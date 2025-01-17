  /*
 * Copyright (c) 2003, 2016, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "cds/metaspaceShared.hpp"
#include "classfile/classFileParser.hpp"
#include "classfile/classFileStream.hpp"
#include "classfile/classLoadInfo.hpp"
#include "classfile/metadataOnStackMark.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/verifier.hpp"
#include "classfile/dictionary.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/loaderConstraints.hpp"
#include "code/nmethod.hpp"
#include "code/codeCache.hpp"
#include "interpreter/linkResolver.hpp"
#include "interpreter/rewriter.hpp"
#include "logging/logStream.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/resourceArea.hpp"
#include "oops/fieldStreams.hpp"
#include "oops/fieldStreams.inline.hpp"
#include "oops/oop.inline.hpp"
#include "oops/constantPool.inline.hpp"
#include "oops/metadata.hpp"
#include "oops/methodData.hpp"
#include "prims/jvmtiImpl.hpp"
#include "prims/jvmtiClassFileReconstituter.hpp"
#include "prims/jvmtiEnhancedRedefineClasses.hpp"
#include "prims/methodComparator.hpp"
#include "prims/resolvedMethodTable.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/relocator.hpp"
#include "runtime/fieldDescriptor.hpp"
#include "runtime/fieldDescriptor.inline.hpp"
#include "utilities/bitMap.inline.hpp"
#include "prims/jvmtiThreadState.inline.hpp"
#include "utilities/events.hpp"
#include "oops/constantPool.inline.hpp"
#if INCLUDE_G1GC
#include "gc/g1/g1CollectedHeap.hpp"
#endif
#include "gc/shared/dcevmSharedGC.hpp"
#include "gc/shared/scavengableNMethods.hpp"
#include "gc/shared/oopStorageSet.hpp"
#include "gc/shared/oopStorageSet.inline.hpp"
#include "gc/shared/weakProcessor.hpp"
#if defined(COMPILER1) || defined(COMPILER2)
#include "ci/ciObjectFactory.hpp"
#endif

#ifdef COMPILER2
#include "opto/c2compiler.hpp"
#endif

Array<Method*>* VM_EnhancedRedefineClasses::_old_methods = nullptr;
Array<Method*>* VM_EnhancedRedefineClasses::_new_methods = nullptr;
Method**  VM_EnhancedRedefineClasses::_matching_old_methods = nullptr;
Method**  VM_EnhancedRedefineClasses::_matching_new_methods = nullptr;
Method**  VM_EnhancedRedefineClasses::_deleted_methods      = nullptr;
Method**  VM_EnhancedRedefineClasses::_added_methods        = nullptr;
int         VM_EnhancedRedefineClasses::_matching_methods_length = 0;
int         VM_EnhancedRedefineClasses::_deleted_methods_length  = 0;
int         VM_EnhancedRedefineClasses::_added_methods_length    = 0;
Klass*      VM_EnhancedRedefineClasses::_the_class_oop = nullptr;
u8        VM_EnhancedRedefineClasses::_id_counter = 0;

//
// Create new instance of enhanced class redefiner.
//
// This class implements VM_GC_Operation - the usual usage should be:
//     VM_EnhancedRedefineClasses op(class_count, class_definitions, jvmti_class_load_kind_redefine);
//     VMThread::execute(&op);
// Which
//  - class_count size of class_defs
//  - class_defs class definition - either new class or redefined class
//               note that this is not the final array of classes to be redefined
//               we need to scan for all affected classes (e.g. subclasses) and
//               calculate redefinition for them as well.
// @param class_load_kind always jvmti_class_load_kind_redefine
VM_EnhancedRedefineClasses::VM_EnhancedRedefineClasses(jint class_count, const jvmtiClassDefinition *class_defs, JvmtiClassLoadKind class_load_kind) :
        VM_GC_Operation(Universe::heap()->total_collections(), GCCause::_heap_inspection, Universe::heap()->total_full_collections(), true) {
  _affected_klasses = nullptr;
  _class_count = class_count;
  _class_defs = class_defs;
  _class_load_kind = class_load_kind;
  _res = JVMTI_ERROR_NONE;
  _any_class_has_resolved_methods = false;
  _object_klass_redefined = false;
  _vm_class_redefined = false;
  _id = next_id();
}

static inline InstanceKlass* get_ik(jclass def) {
  oop mirror = JNIHandles::resolve_non_null(def);
  return InstanceKlass::cast(java_lang_Class::as_Klass(mirror));
}

// Start the redefinition:
// - Load new class definitions - @see load_new_class_versions
// - Start mark&sweep GC.
// - true if success, otherwise all chnages are rollbacked.
bool VM_EnhancedRedefineClasses::doit_prologue() {

  if (_class_count == 0) {
    _res = JVMTI_ERROR_NONE;
    return false;
  }
  if (_class_defs == nullptr) {
    _res = JVMTI_ERROR_NULL_POINTER;
    return false;
  }
  for (int i = 0; i < _class_count; i++) {
    if (_class_defs[i].klass == nullptr) {
      _res = JVMTI_ERROR_INVALID_CLASS;
      return false;
    }
    if (_class_defs[i].class_byte_count == 0) {
      _res = JVMTI_ERROR_INVALID_CLASS_FORMAT;
      return false;
    }
    if (_class_defs[i].class_bytes == nullptr) {
      _res = JVMTI_ERROR_NULL_POINTER;
      return false;
    }

    // classes for primitives and arrays and vm anonymous classes cannot be redefined
    // check here so following code can assume these classes are InstanceKlass
    oop mirror = JNIHandles::resolve_non_null(_class_defs[i].klass);
    if (!is_modifiable_class(mirror)) {
      _res = JVMTI_ERROR_UNMODIFIABLE_CLASS;
      return false;
    }
  }

  // Start timer after all the sanity checks; not quite accurate, but
  // better than adding a bunch of stop() calls.
  if (log_is_enabled(Info, redefine, class, timer)) {
    _timer_vm_op_prologue.start();
  }

  // We first load new class versions in the prologue, because somewhere down the
  // call chain it is required that the current thread is a Java thread.
  _res = load_new_class_versions(JavaThread::current());

  // prepare GC, lock heap
  _full_gc_count_before = Universe::heap()->total_full_collections(); // force gc lock in next VM_GC_Operation::doit_prologue()
  if (_res == JVMTI_ERROR_NONE && !VM_GC_Operation::doit_prologue()) {
    _res = JVMTI_ERROR_INTERNAL;
  }

  if (_res != JVMTI_ERROR_NONE) {
    rollback();
    // TODO free any successfully created classes
    /*for (int i = 0; i < _class_count; i++) {
      if (_new_classes[i] != nullptr) {
        ClassLoaderData* cld = _new_classes[i]->class_loader_data();
        // Free the memory for this class at class unloading time.  Not before
        // because CMS might think this is still live.
        cld->add_to_deallocate_list(InstanceKlass::cast(_new_classes[i]));
      }
    }*/
    delete _new_classes;
    _new_classes = nullptr;
    delete _affected_klasses;
    _affected_klasses = nullptr;

    _timer_vm_op_prologue.stop();
    return false;
  }

  _timer_vm_op_prologue.stop();
  return true;
}

// Closer for static fields - copy value from old class to the new class.
class FieldCopier : public FieldClosure {
  public:
  void do_field(fieldDescriptor* fd) {
    InstanceKlass* cur = InstanceKlass::cast(fd->field_holder());
    oop cur_oop = cur->java_mirror();

    InstanceKlass* old = InstanceKlass::cast(cur->old_version());
    oop old_oop = old->java_mirror();

    fieldDescriptor result;
    bool found = old->find_local_field(fd->name(), fd->signature(), &result);
    if (found && result.is_static()) {
      log_trace(redefine, class, obsolete, metadata)("Copying static field value for field %s old_offset=%d new_offset=%d",
                                               fd->name()->as_C_string(), result.offset(), fd->offset());
      memcpy(cur_oop->field_addr<HeapWord>(fd->offset()),
             old_oop->field_addr<HeapWord>(result.offset()),
             type2aelembytes(fd->field_type()));

      // Static fields may have references to java.lang.Class
      if (fd->field_type() == T_OBJECT) {
         oop oop = cur_oop->obj_field(fd->offset());
         if (oop != nullptr && oop->is_instance() && InstanceKlass::cast(oop->klass())->is_mirror_instance_klass()) {
            Klass* klass = java_lang_Class::as_Klass(oop);
            if (klass != nullptr && klass->is_instance_klass()) {
              assert(oop == InstanceKlass::cast(klass)->java_mirror(), "just checking");
              if (klass->new_version() != nullptr) {
                oop = InstanceKlass::cast(klass->new_version())->java_mirror();
                cur_oop->obj_field_put(fd->offset(), oop);
              }
            }
          }
        }
      }
    }
};


// TODO: review...
void VM_EnhancedRedefineClasses::mark_as_scavengable(nmethod* nm) {
  ScavengableNMethods::register_nmethod(nm);
}

void VM_EnhancedRedefineClasses::unregister_nmethod_g1(nmethod* nm) {
  // It should work not only for G1 but also for another GCs, but this way is safer now
  Universe::heap()->unregister_nmethod(nm);
}

void VM_EnhancedRedefineClasses::register_nmethod_g1(nmethod* nm) {
  // It should work not only for G1 but also for another GCs, but this way is safer now
  Universe::heap()->register_nmethod(nm);
}

void VM_EnhancedRedefineClasses::root_oops_do(OopClosure *oopClosure) {
  Universe::vm_global()->oops_do(oopClosure);

  // TODO: cld iteration fails, explain why?
  // CLDToOopClosure cldToOopClosure(oopClosure, ClassLoaderData::_claim_none);
  // ClassLoaderDataGraph::roots_cld_do(&cldToOopClosure, nullptr);

  Threads::oops_do(oopClosure, nullptr);
  OopStorageSet::strong_oops_do(oopClosure);
  WeakProcessor::oops_do(oopClosure);

  CodeBlobToOopClosure blobClosure(oopClosure, CodeBlobToOopClosure::FixRelocations);
  CodeCache::blobs_do_dcevm(&blobClosure);
  if (ScavengeRootsInCode && !UseG1GC) {
    // Mark scavengable again since class oops could be updated,
    CodeCache::nmethods_do(mark_as_scavengable);
  }
}

// TODO comment
struct StoreBarrier {
  // TODO: j10 review change ::oop_store -> HeapAccess<>::oop_store
  template <class T> static void oop_store_not_null(T* p, oop v) { HeapAccess<IS_NOT_NULL>::oop_store(p, v); }
  template <class T> static void oop_store(T* p) { HeapAccess<>::oop_store(p, oop(nullptr)); }
};


// TODO comment
struct StoreNoBarrier {
  template <class T> static void oop_store_not_null(T* p, oop v) { RawAccess<IS_NOT_NULL>::oop_store(p, v); }
  template <class T> static void oop_store(T* p) { RawAccess<>::oop_store(p, oop(nullptr)); }
};

// Closure to scan all heap objects and update method handles
template <class S>
class ChangePointersOopClosure : public BasicOopIterateClosure {
  // import java_lang_invoke_MemberName.*
  enum {
    REFERENCE_KIND_SHIFT = java_lang_invoke_MemberName::MN_REFERENCE_KIND_SHIFT,
    REFERENCE_KIND_MASK  = java_lang_invoke_MemberName::MN_REFERENCE_KIND_MASK,
  };

  bool update_member_name(oop obj) {
    int flags    =       java_lang_invoke_MemberName::flags(obj);
    int ref_kind =       (flags >> REFERENCE_KIND_SHIFT) & REFERENCE_KIND_MASK;
    if (MethodHandles::ref_kind_is_method(ref_kind)) {
      Method* m = (Method*) java_lang_invoke_MemberName::vmtarget(obj);
      if (m != nullptr && m->method_holder()->is_redefining()) {
        // Let's try to re-resolve method
        InstanceKlass* newest = InstanceKlass::cast(m->method_holder()->newest_version());
        Method* new_method = newest->find_method(m->name(), m->signature());

        if (new_method != nullptr) {
          // Note: we might set nullptr at this point, which should force AbstractMethodError at runtime
          Thread *current = Thread::current();
          CallInfo info(new_method, newest, current);
          oop resolved_method = ResolvedMethodTable::find_method(info.resolved_method());
          if (resolved_method != nullptr) {
            info.set_resolved_method_name_dcevm(resolved_method, current);
            Handle objHandle(current, obj);
            MethodHandles::init_method_MemberName(objHandle, info);
          } else {
            assert(0, "Must be resolved");
            java_lang_invoke_MemberName::set_method(obj, nullptr);
          }
        } else {
          java_lang_invoke_MemberName::set_method(obj, nullptr);
        }
      }
    } else if (MethodHandles::ref_kind_is_field(ref_kind)) {
      oop clazz = java_lang_invoke_MemberName::clazz(obj);
      if (clazz == nullptr) {
        return false;
      }
      Klass* k = java_lang_Class::as_Klass(clazz);
      if (k == nullptr) {
        return false; // Was cleared before, this MemberName is invalid.
      }

      if (k->is_redefining()) {
        // Let's try to re-resolve field
        InstanceKlass* old = InstanceKlass::cast(k->old_version());
        fieldDescriptor fd;
        int offset = java_lang_invoke_MemberName::vmindex(obj);
        bool is_static = MethodHandles::ref_kind_is_static(ref_kind);
        InstanceKlass* ik_old = InstanceKlass::cast(old);
        if (ik_old->find_local_field_from_offset(offset, is_static, &fd)) {
          InstanceKlass* ik_new = InstanceKlass::cast(k->newest_version());
          fieldDescriptor fd_new;
          if (ik_new->find_local_field(fd.name(), fd.signature(), &fd_new)) {
            Handle objHandle(Thread::current(), obj);
            MethodHandles::init_field_MemberName(objHandle, fd_new, MethodHandles::ref_kind_is_setter(ref_kind));
          } else {
            // Matching field is not found in new version, not much we can do here.
            // JVM will crash once faulty MH is invoked.
            // However, to avoid that all DMH's using this faulty MH are cleared (set to nullptr)
            // Eventually, we probably want to replace them with something more meaningful,
            // like instance throwing NoSuchFieldError or DMH that will resort to dynamic
            // field resolution (with possibility of type conversion)
            java_lang_invoke_MemberName::set_clazz(obj, nullptr);
            java_lang_invoke_MemberName::set_vmindex(obj, 0);
            return false;
          }
        }
      }
    }
    return true;
  }

  bool update_direct_method_handle(oop obj) {
    // Always update member name first.
    oop mem_name = java_lang_invoke_DirectMethodHandle::member(obj);
    if (mem_name == nullptr) {
      return true;
    }
    if (!update_member_name(mem_name)) {
      return false;
    }

    // Here we rely on DirectMethodHandle implementation.
    // The current implementation caches field offset in $StaticAccessor/$Accessor
    int flags    =       java_lang_invoke_MemberName::flags(mem_name);
    int ref_kind =       (flags >> REFERENCE_KIND_SHIFT) & REFERENCE_KIND_MASK;
    if (MethodHandles::ref_kind_is_field(ref_kind)) {
      // Note: we don't care about staticBase field (which is java.lang.Class)
      // It should be processed during normal object update.
      // Update offset in StaticAccessor
      int offset = java_lang_invoke_MemberName::vmindex(mem_name);
      if (offset != 0) { // index of 0 means that field no longer exist
        if (java_lang_invoke_DirectMethodHandle_StaticAccessor::is_instance(obj)) {
          java_lang_invoke_DirectMethodHandle_StaticAccessor::set_static_offset(obj, offset);
        } else if (java_lang_invoke_DirectMethodHandle_Accessor::is_instance(obj)) {
          java_lang_invoke_DirectMethodHandle_Accessor::set_field_offset(obj, offset);
        }
      }
    }
    return true;
  }

  // Forward pointers to InstanceKlass and mirror class to new versions
  template <class T>
  inline void do_oop_work(T* p) {
    oop obj = RawAccess<>::oop_load(p);
    if (obj == nullptr) {
      return;
    }
    bool oop_updated  = false;
    if (obj->is_instance() && InstanceKlass::cast(obj->klass())->is_mirror_instance_klass()) {
      Klass* klass = java_lang_Class::as_Klass(obj);
      if (klass != nullptr && klass->is_instance_klass() && klass->new_version() != nullptr) {
        assert(obj == InstanceKlass::cast(klass)->java_mirror(), "just checking");
        if (klass->new_version() != nullptr) {
          obj = InstanceKlass::cast(klass->new_version())->java_mirror();
          S::oop_store_not_null(p, obj);
          oop_updated = true;
        }
      }
    }

    // JSR 292 support, uptade java.lang.invoke.MemberName instances
    if (java_lang_invoke_MemberName::is_instance(obj)) {
      if (oop_updated) {
        update_member_name(obj);
      }
    } else if (java_lang_invoke_DirectMethodHandle::is_instance(obj)) {
      if (!update_direct_method_handle(obj)) {
        // DMH is no longer valid, replace it with null reference.
        // See note above. We probably want to replace this with something more meaningful.
        S::oop_store(p);
      }
    }
  }

  virtual void do_oop(oop* o) {
    do_oop_work(o);
  }

  virtual void do_oop(narrowOop* o) {
    do_oop_work(o);
  }
};

// Closure to scan all objects on heap for objects of changed classes
//  - if the fields are compatible, only update class definition reference
//  - otherwise if the new object size is smaller than old size, reshufle
//         the fields and fill the gap with "dead_space"
//  - otherwise set the _needs_instance_update flag, we need to do full GC
//         and reshuffle object positions durring mark&sweep
class ChangePointersObjectClosure : public ObjectClosure {
  private:

  OopIterateClosure *_closure;
  bool _needs_instance_update;
  oop _tmp_obj;
  size_t _tmp_obj_size;

public:
  ChangePointersObjectClosure(OopIterateClosure *closure) : _closure(closure), _needs_instance_update(false), _tmp_obj(nullptr), _tmp_obj_size(0) {}

  bool needs_instance_update() {
    return _needs_instance_update;
  }

  void copy_to_tmp(oop o) {
    size_t size = o->size();
    if (_tmp_obj_size < size) {
      _tmp_obj_size = size;
      _tmp_obj = cast_to_oop(resource_allocate_bytes(size * HeapWordSize));
    }
    Copy::aligned_disjoint_words(cast_from_oop<HeapWord*>(o), cast_from_oop<HeapWord*>(_tmp_obj), size);
  }

  virtual void do_object(oop obj) {
    if (obj->is_instance() && InstanceKlass::cast(obj->klass())->is_mirror_instance_klass()) {
      // static fields may have references to old java.lang.Class instances, update them
      // at the same time, we don't want to update other oops in the java.lang.Class
      // Causes SIGSEGV?
      //instanceMirrorKlass::oop_fields_iterate(obj, _closure);
    } else {
      obj->oop_iterate(_closure);
    }

    if (obj->klass()->new_version() != nullptr) {
      Klass* new_klass = obj->klass()->new_version();

      if (new_klass->update_information() != nullptr) {
        if (obj->size() - obj->size_given_klass(new_klass) != 0) {
          // We need an instance update => set back to old klass
          _needs_instance_update = true;
        } else {
          // Either new size is bigger or gap is too small to be filled
          oop src = obj;
          if (new_klass->is_copying_backwards()) {
            copy_to_tmp(obj);
            src = _tmp_obj;
          }
          src->set_klass(obj->klass()->new_version());
          //  FIXME: instance updates...
          //guarantee(false, "instance updates!");
          DcevmSharedGC::update_fields(obj, src, new_klass->update_information());
        }
      } else {
        obj->set_klass(obj->klass()->new_version());
      }
    }
  }
};

class ChangePointersObjectTask : public WorkerTask {
private:
  ChangePointersOopClosure<StoreBarrier>* _cl;
  ParallelObjectIterator* _poi;
  bool _needs_instance_update;
public:
  ChangePointersObjectTask(ChangePointersOopClosure<StoreBarrier>* cl, ParallelObjectIterator* poi) : WorkerTask("IterateObject Closure"),
                                                                                                      _cl(cl), _poi(poi), _needs_instance_update(false) { }

  virtual void work(uint worker_id) {
    HandleMark hm(Thread::current());   // make sure any handles created are deleted
    ChangePointersObjectClosure objectClosure(_cl);
    _poi->object_iterate(&objectClosure, worker_id);
    _needs_instance_update = _needs_instance_update || objectClosure.needs_instance_update();
  }
  bool needs_instance_update() {
    return _needs_instance_update;
  }
};

// Main transformation method - runs in VM thread.
//  - for each scratch class call redefine_single_class
//  - clear code cache (flush_dependent_code)
//  - iterate the heap and update object definitions, check it old/new class fields
//       are compatible. If new class size is smaller than old, it can be solved directly here.
//  - iterate the heap and update method handles to new version
//  - Swap marks to have same hashcodes
//  - copy static fields
//  - notify JVM of the modification
void VM_EnhancedRedefineClasses::doit() {
  Thread *current = Thread::current();

#if INCLUDE_CDS
  if (UseSharedSpaces) {
    // Sharing is enabled so we remap the shared readonly space to
    // shared readwrite, private just in case we need to redefine
    // a shared class. We do the remap during the doit() phase of
    // the safepoint to be safer.
    if (!MetaspaceShared::remap_shared_readonly_as_readwrite()) {
      log_info(redefine, class, load)("failed to remap shared readonly space to readwrite, private");
      _res = JVMTI_ERROR_INTERNAL;
      return;
    }
  }
#endif

  if (log_is_enabled(Info, redefine, class, timer)) {
    _timer_vm_op_doit.start();
  }

  Universe::set_inside_redefinition(true);

  // Mark methods seen on stack and everywhere else so old methods are not
  // cleaned up if they're on the stack.

  // FIXME: fails in enhanced redefinition
  // MetadataOnStackMark md_on_stack(true);
  HandleMark hm(current);   // make sure any handles created are deleted
                           // before the stack walk again.

  for (int i = 0; i < _new_classes->length(); i++) {
    redefine_single_class(current, _new_classes->at(i));
  }

  // Update vmClasses::*_klass
  for (int i = 0; i < _new_classes->length(); i++) {
    InstanceKlass* cur = _new_classes->at(i);
    if (vmClasses::update_vm_klass(InstanceKlass::cast(cur->old_version()), cur)) {
      _vm_class_redefined = true;
      log_trace(redefine, class, obsolete, metadata)("Well known class updated %s", cur->external_name());
#if defined(COMPILER1) || defined(COMPILER2)
      ciObjectFactory::set_reinitialize_vm_klasses();
#endif
#ifdef COMPILER2
      C2Compiler::set_reinitialize_vm_klasses();
#endif
    }
  }

  // Update vmClasses in universe, after update vmClasses
  Universe::update_vmClasses_dcevm();

  // Deoptimize all compiled code that depends on this class (do only once, because it clears whole cache)
  // if (_max_redefinition_flags > Klass::ModifyClass) {
    flush_dependent_code();
  // }

  // Adjust constantpool caches for all classes that reference methods of the evolved class.
  ClearCpoolCacheAndUnpatch clear_cpool_cache(current);
  ClassLoaderDataGraph::classes_do(&clear_cpool_cache);

  // JSR-292 support
  if (_any_class_has_resolved_methods) {
    bool trace_name_printed = false;
    ResolvedMethodTable::adjust_method_entries_dcevm(&trace_name_printed);
  }

  ChangePointersOopClosure<StoreNoBarrier> oopClosureNoBarrier;
  ChangePointersOopClosure<StoreBarrier> oopClosure;
  bool needs_instance_update = false;

  log_trace(redefine, class, redefine, metadata)("Before updating instances");
  {
    // Since we may update oops inside nmethod's code blob to point to java.lang.Class in new generation, we need to
    // make sure such references are properly recognized by GC. For that, If ScavengeRootsInCode is true, we need to
    // mark such nmethod's as "scavengable".
    // For now, mark all nmethod's as scavengable that are not scavengable already
    if (ScavengeRootsInCode) {
#if INCLUDE_G1GC
      if (UseG1GC) {
        // G1 holds references to nmethods in regions based on oops values. Since oops in nmethod can be changed in ChangePointers* closures
        // we unregister nmethods from G1 heap, then closures are processed (oops are changed) and finally we register nmethod to G1 again
        CodeCache::nmethods_do(unregister_nmethod_g1);
      } else {
#endif
        CodeCache::nmethods_do(mark_as_scavengable);
#if INCLUDE_G1GC
      }
#endif
    }

    if (log_is_enabled(Info, redefine, class, timer)) {
      _timer_heap_iterate.start();
    }
    Universe::heap()->ensure_parsability(false);
    WorkerThreads* workers = Universe::heap()->safepoint_workers();
    if (workers != nullptr && workers->active_workers() > 1) {
      ParallelObjectIterator poi(workers->active_workers());
      ChangePointersObjectTask objectTask(&oopClosure, &poi);
      workers->run_task(&objectTask);
      needs_instance_update = objectTask.needs_instance_update();
    } else {
      ChangePointersObjectClosure objectClosure(&oopClosure);
      Universe::heap()->object_iterate(&objectClosure);
      needs_instance_update = objectClosure.needs_instance_update();
    }

    root_oops_do(&oopClosureNoBarrier);

    _timer_heap_iterate.stop();

#if INCLUDE_G1GC
    if (UseG1GC) {
      // this should work also for other GCs
      CodeCache::nmethods_do(register_nmethod_g1);
    }
#endif

  }
  log_trace(redefine, class, redefine, metadata)("After updating instances");

  for (int i = 0; i < _new_classes->length(); i++) {
    InstanceKlass* cur = InstanceKlass::cast(_new_classes->at(i));
    InstanceKlass* old = InstanceKlass::cast(cur->old_version());

    // Swap marks to have same hashcodes
    markWord cur_mark = cur->java_mirror()->mark();
    markWord old_mark = old->java_mirror()->mark();
    cur->java_mirror()->set_mark(old_mark);
    old->java_mirror()->set_mark(cur_mark);


    // Revert pool holder for old version of klass (it was updated by one of ours closure!)
    old->constants()->set_pool_holder(old);

    // Initialize the new class! Special static initialization that does not execute the
    // static constructor but copies static field values from the old class if name
    // and signature of a static field match.
    FieldCopier copier;
    cur->do_local_static_fields(&copier); // TODO (tw): What about internal static fields??
    //java_lang_Class::set_klass(old->java_mirror(), cur); // FIXME-isd (from JDK8): is that correct?
    //FIXME-isd (from JDK8): do we need this: ??? old->set_java_mirror(cur->java_mirror());

    // Transfer init state
    InstanceKlass::ClassState state = old->init_state();
    if (state > InstanceKlass::linked) {
      cur->set_init_state(state);
    }
  }

  // Update objArrayKlasses
  for (int i = 0; i < _new_classes->length(); i++) {
    InstanceKlass* cur = InstanceKlass::cast(_new_classes->at(i));
    InstanceKlass* old = InstanceKlass::cast(cur->old_version());

    ObjArrayKlass* array_klasses = old->array_klasses();
    if (array_klasses != nullptr) {
      // Transfer the array classes, otherwise we might get cast exceptions when casting array types.
      // Also, update element klass and bottom class
      cur->release_set_array_klasses(array_klasses);
      array_klasses->update_supers_dcevm();
      array_klasses->set_element_klass(cur);
      array_klasses->set_bottom_klass(array_klasses->bottom_klass()->newest_version());
      java_lang_Class::release_set_array_klass(cur->java_mirror(), array_klasses);
      java_lang_Class::set_component_mirror(array_klasses->java_mirror(), cur->java_mirror());

      int dim = 2;
      Klass* klass = array_klasses->array_klass_or_null(dim);
      while (klass != nullptr) {
        ObjArrayKlass *array_klass2 = ObjArrayKlass::cast(klass);
        array_klass2->update_supers_dcevm();
        klass = cur->array_klass_or_null(++dim);
      }
    }
  }

  if (_object_klass_redefined) {
    for (int i = T_BOOLEAN; i < T_LONG+1; i++) {
      TypeArrayKlass* array_klass = TypeArrayKlass::cast(Universe::typeArrayKlassObj((BasicType)i));
      array_klass->update_supers_dcevm();
      array_klass->append_to_sibling_list();
      assert(array_klass->is_array_klass() && array_klass->is_typeArray_klass(), "Must be type array class");
    }
    Universe::objectArrayKlassObj()->append_to_sibling_list();
  }

  if (_object_klass_redefined) {
    // TODO: This is a hack; it keeps old mirror instances on the heap. A correct solution could be to hold the old mirror class in the new mirror class.
    ClassUnloading = false;
    ClassUnloadingWithConcurrentMark = false;
  }

  if (needs_instance_update) {
    // Do a full garbage collection to update the instance sizes accordingly
    log_trace(redefine, class, redefine, metadata)("Before redefinition full GC run");

    if (log_is_enabled(Info, redefine, class, timer)) {
      _timer_heap_full_gc.start();
    }

    Universe::set_redefining_gc_run(true);
    notify_gc_begin(true);
    // TODO: check _metadata_GC_clear_soft_refs with ScavengeRootsInCode
    Universe::heap()->collect_as_vm_thread(GCCause::_heap_inspection);
    notify_gc_end();
    Universe::set_redefining_gc_run(false);

    _timer_heap_full_gc.stop();
    log_trace(redefine, class, redefine, metadata)("After redefinition full GC run");
  }

  // Unmark Klasses as "redefining"
  for (int i = 0; i < _new_classes->length(); i++) {
    InstanceKlass* cur = _new_classes->at(i);
    cur->set_redefining(false);
    cur->clear_update_information();
  }

  // TODO: explain...
  LoaderConstraintTable::update_after_redefinition();

  // TODO: explain...
#if defined(COMPILER1) || defined(COMPILER2)
  ciObjectFactory::resort_shared_ci_metadata();
#endif

  // FIXME - check if it was in JDK8. Copied from standard JDK9 hotswap.
  //MethodDataCleaner clean_weak_method_links;
  //ClassLoaderDataGraph::classes_do(&clean_weak_method_links);

  JvmtiExport::increment_redefinition_count();

#ifdef PRODUCT
  if (log_is_enabled(Trace, redefine, class, obsolete, metadata)) {
#endif
  for (int i=0; i<_affected_klasses->length(); i++) {
    Klass* the_class = _affected_klasses->at(i);
    if (the_class != nullptr) {
      assert(the_class->new_version() != nullptr, "Must have been redefined");
      Klass *new_version = the_class->new_version();
      assert(new_version->new_version() == nullptr, "Must be newest version");

      if (!(new_version->super() == nullptr || new_version->super()->new_version() == nullptr)) {
        new_version->print();
        new_version->super()->print();
      }
      assert(new_version->super() == nullptr || new_version->super()->new_version() == nullptr, "Super class must be newest version");
    }
  }
  log_trace(redefine, class, redefine, metadata)("calling check_class");
  ClassLoaderData::the_null_class_loader_data()->dictionary()->classes_do_safepoint(check_class);
#ifdef PRODUCT
  }
#endif

  Universe::set_inside_redefinition(false);
  _timer_vm_op_doit.stop();
}

// Cleanup - runs in JVM thread
//  - free used memory
//  - end GC
void VM_EnhancedRedefineClasses::doit_epilogue() {
  VM_GC_Operation::doit_epilogue();

  if (!_new_classes->is_empty() && _vm_class_redefined) {
    ResourceMark rm(JavaThread::current());
    log_trace(redefine, class, obsolete, metadata)("Reinitialize known methods in universe.");
    Universe::reinitialize_known_method_dcevm(JavaThread::current());
  }

  if (_new_classes != nullptr) {
    delete _new_classes;
  }
  _new_classes = nullptr;
  if (_affected_klasses != nullptr) {
    delete _affected_klasses;
  }
  _affected_klasses = nullptr;

  // Reset the_class_oop to null for error printing.
  _the_class_oop = nullptr;

  if (log_is_enabled(Info, redefine, class, timer)) {
    // Used to have separate timers for "doit" and "all", but the timer
    // overhead skewed the measurements.
    jlong all_time = _timer_vm_op_prologue.milliseconds() + _timer_vm_op_doit.milliseconds();

    log_info(redefine, class, timer)
      ("vm_op: all=" JLONG_FORMAT "  prologue=" JLONG_FORMAT "  doit=" JLONG_FORMAT,
       all_time, _timer_vm_op_prologue.milliseconds(), _timer_vm_op_doit.milliseconds());
    log_info(redefine, class, timer)
      ("doit: heap iterate=" JLONG_FORMAT "  fullgc=" JLONG_FORMAT,
       _timer_heap_iterate.milliseconds(), _timer_heap_full_gc.milliseconds());
  }
}

// Exclude java primitives and arrays from redefinition
//  - klass_mirror  pointer to the klass
//  - true if is modifiable
bool VM_EnhancedRedefineClasses::is_modifiable_class(oop klass_mirror) {
  // classes for primitives cannot be redefined
  if (java_lang_Class::is_primitive(klass_mirror)) {
    return false;
  }
  Klass* k = java_lang_Class::as_Klass(klass_mirror);
  // classes for arrays cannot be redefined
  if (k == nullptr || !k->is_instance_klass()) {
    return false;
  }

  // Cannot redefine or retransform an anonymous class.
  // TODO: check if is correct in j15
  if (InstanceKlass::cast(k)->is_hidden()) {
    return false;
  }
  return true;
}

// Load and link new classes (either redefined or affected by redefinition - subclass, ...)
//  - find sorted affected classes
//  - resolve new class
//  - calculate redefine flags (field change, method change, supertype change, ...)
//  - calculate modified fields and mapping to old fields
//  - link new classes
jvmtiError VM_EnhancedRedefineClasses::load_new_class_versions(TRAPS) {

  _affected_klasses = new (mtInternal) GrowableArray<Klass*>(_class_count, mtInternal);
  _new_classes = new (mtInternal) GrowableArray<InstanceKlass*>(_class_count, mtInternal);

  ResourceMark rm(THREAD);

  // Retrieve an array of all classes that need to be redefined into _affected_klasses
  jvmtiError err = find_sorted_affected_classes(true, nullptr, THREAD);
  if (err != JVMTI_ERROR_NONE) {
    return err;
  }

  _max_redefinition_flags = Klass::NoRedefinition;

  GrowableArray<Klass*>* prev_affected_klasses = new (mtInternal) GrowableArray<Klass*>(_class_count, mtInternal);

  do {
    err = load_new_class_versions_single_step(THREAD);
    if (err != JVMTI_ERROR_NONE) {
      delete prev_affected_klasses;
      return err;
    }

    {
      GrowableArray<Klass*>* store = prev_affected_klasses;
      prev_affected_klasses = _affected_klasses;
      _affected_klasses = store;
      _affected_klasses->clear();
    }

    err = find_sorted_affected_classes(false, prev_affected_klasses, THREAD);
    if (err != JVMTI_ERROR_NONE) {
      delete prev_affected_klasses;
      return err;
    }
  } while(_affected_klasses->length() != prev_affected_klasses->length());

  delete _affected_klasses;
  _affected_klasses = prev_affected_klasses;

  // Link and verify new classes _after_ all classes have been updated in the system dictionary!
  for (int i = 0; i < _affected_klasses->length(); i++) {
    Klass* the_class = _affected_klasses->at(i);
    if (the_class != nullptr) {
      assert (the_class->new_version() != nullptr, "new version must be present");
      InstanceKlass *new_class(InstanceKlass::cast(the_class->new_version()));
      new_class->link_class(THREAD);
      if (HAS_PENDING_EXCEPTION) {
        Symbol *ex_name = PENDING_EXCEPTION->klass()->name();
        log_info(redefine, class, load, exceptions)("link_class exception: '%s'", new_class->name()->as_C_string());
        CLEAR_PENDING_EXCEPTION;
        if (ex_name == vmSymbols::java_lang_OutOfMemoryError()) {
          return JVMTI_ERROR_OUT_OF_MEMORY;
        } else {
          return JVMTI_ERROR_INTERNAL;
        }
      }
    }
  }
  return JVMTI_ERROR_NONE;
}

jvmtiError VM_EnhancedRedefineClasses::load_new_class_versions_single_step(TRAPS) {

  // thread local state - used to transfer class_being_redefined object to SystemDictonery::resolve_from_stream
  JvmtiThreadState *state = JvmtiThreadState::state_for(JavaThread::current());
  // state can only be nullptr if the current thread is exiting which
  // should not happen since we're trying to do a RedefineClasses
  guarantee(state != nullptr, "exiting thread calling load_new_class_versions");

  for (int i = 0; i < _affected_klasses->length(); i++) {
    // Create HandleMark so that any handles created while loading new class
    // versions are deleted. Constant pools are deallocated while merging
    // constant pools
    HandleMark hm(THREAD);
    InstanceKlass* the_class = InstanceKlass::cast(_affected_klasses->at(i));

    if (the_class->new_version() != nullptr) {
      continue;
    }

    Symbol*  the_class_sym = the_class->name();

    // Ensure class is linked before redefine
    if (!the_class->is_linked()) {
      the_class->link_class(THREAD);
      if (HAS_PENDING_EXCEPTION) {
        Symbol* ex_name = PENDING_EXCEPTION->klass()->name();
        oop message = java_lang_Throwable::message(PENDING_EXCEPTION);
        if (message != nullptr) {
          char* ex_msg = java_lang_String::as_utf8_string(message);
          log_info(redefine, class, load, exceptions)("link_class exception: '%s %s'", ex_name->as_C_string(), ex_msg);
        } else {
          log_info(redefine, class, load, exceptions)("link_class exception: '%s'", ex_name->as_C_string());
        }
        CLEAR_PENDING_EXCEPTION;
        if (ex_name == vmSymbols::java_lang_OutOfMemoryError()) {
          return JVMTI_ERROR_OUT_OF_MEMORY;
        } else if (ex_name == vmSymbols::java_lang_NoClassDefFoundError()) {
          if (the_class->check_redefinition_flag(Klass::PrimaryRedefine)) {
            return JVMTI_ERROR_INTERNAL;
          }
          _affected_klasses->at_put(i, nullptr);
          continue;
        } else {
          return JVMTI_ERROR_INTERNAL;
        }
      }
    }

    log_debug(redefine, class, load)
      ("loading name=%s kind=%d (avail_mem=" UINT64_FORMAT "K)",
       the_class->external_name(), _class_load_kind, os::available_memory() >> 10);

    // class bytes...
    const unsigned char* class_bytes;
    jint class_byte_count;
    jvmtiError error;
    jboolean not_changed;
    if ((error = find_class_bytes(the_class, &class_bytes, &class_byte_count, &not_changed)) != JVMTI_ERROR_NONE) {
      log_info(redefine, class, load, exceptions)("error finding class bytes: %d", (int) error);
      return error;
    }
    assert(class_bytes != nullptr && class_byte_count != 0, "class bytes should be defined at this point!");

    ClassFileStream st((u1*)class_bytes,
                       class_byte_count,
                       "__VM_EnhancedRedefineClasses__",
                       ClassFileStream::verify);

    // Parse the stream.
    Handle the_class_loader(THREAD, the_class->class_loader());
    Handle protection_domain(THREAD, the_class->protection_domain());
    // Set redefined class handle in JvmtiThreadState class.
    // This redefined class is sent to agent event handler for class file
    // load hook event.
    state->set_class_being_redefined(the_class, _class_load_kind);

    InstanceKlass* k;

    if (the_class->is_hidden()) {
      log_debug(redefine, class, load)("loading hidden class %s", the_class->name()->as_C_string());
      InstanceKlass* dynamic_host_class = the_class->nest_host(THREAD);

      ClassLoadInfo cl_info(protection_domain,
                            dynamic_host_class,     // dynamic_nest_host
                            Handle(), // classData
                            the_class->is_hidden(),    // is_hidden
                            !the_class->is_non_strong_hidden(),    // is_strong_hidden
                            true);    // FIXME: check if correct. can_access_vm_annotations

      k = SystemDictionary::resolve_from_stream(&st,
                                                the_class_sym,
                                                the_class_loader,
                                                cl_info,
                                                the_class,
                                                THREAD);

      if (!HAS_PENDING_EXCEPTION) {
        if (the_class->class_loader_data()->holder_no_keepalive() != nullptr) {
          k->class_loader_data()->exchange_holders(the_class->class_loader_data());
        }

        // TODO: (DCEVM) review if is correct
        // from jvm_lookup_define_class() (jvm.cpp):
        // The hidden class loader data has been artificially been kept alive to
        // this point. The mirror and any instances of this class have to keep
        // it alive afterwards.
        if (the_class->class_loader_data()->keep_alive_cnt() > 0) {
          the_class->class_loader_data()->dec_keep_alive();
        }
      }

    } else {
      ClassLoadInfo cl_info(protection_domain,
                            nullptr,     // dynamic_nest_host
                            Handle(), // classData
                            the_class->is_hidden(),    // is_hidden
                            !the_class->is_non_strong_hidden(),    // is_strong_hidden
                            true);    // FIXME: check if correct. can_access_vm_annotations
      k = SystemDictionary::resolve_from_stream(&st,
                                                the_class_sym,
                                                the_class_loader,
                                                cl_info,
                                                the_class,
                                                THREAD);
    }
    // Clear class_being_redefined just to be sure.
    state->clear_class_being_redefined();

    if (HAS_PENDING_EXCEPTION) {
      Symbol* ex_name = PENDING_EXCEPTION->klass()->name();
      log_info(redefine, class, load, exceptions)("parse_stream exception: '%s'", ex_name->as_C_string());
      CLEAR_PENDING_EXCEPTION;

      if (ex_name == vmSymbols::java_lang_UnsupportedClassVersionError()) {
        return JVMTI_ERROR_UNSUPPORTED_VERSION;
      } else if (ex_name == vmSymbols::java_lang_ClassFormatError()) {
        return JVMTI_ERROR_INVALID_CLASS_FORMAT;
      } else if (ex_name == vmSymbols::java_lang_ClassCircularityError()) {
        return JVMTI_ERROR_CIRCULAR_CLASS_DEFINITION;
      } else if (ex_name == vmSymbols::java_lang_NoClassDefFoundError()) {
        if (the_class->check_redefinition_flag(Klass::PrimaryRedefine)) {
          return JVMTI_ERROR_NAMES_DONT_MATCH;
        }
        if (k != nullptr) {
          SystemDictionary::remove_from_hierarchy(k);
          k->set_redefining(false);
          k->old_version()->set_new_version(nullptr);
          k->set_old_version(nullptr);
        }
        _affected_klasses->at_put(i, nullptr);
        continue;
      } else if (ex_name == vmSymbols::java_lang_OutOfMemoryError()) {
        return JVMTI_ERROR_OUT_OF_MEMORY;
      } else {  // Just in case more exceptions can be thrown.
        return JVMTI_ERROR_FAILS_VERIFICATION;
      }
    }

    the_class->clear_redefinition_flag(Klass::PrimaryRedefine);

    InstanceKlass* new_class = k;
    the_class->set_new_version(new_class);
    _new_classes->append(new_class);

    if (the_class == vmClasses::Reference_klass()) {
      // must set offset+count to skip field "referent". Look at InstanceRefKlass::update_nonstatic_oop_maps
      OopMapBlock* old_map = the_class->start_of_nonstatic_oop_maps();
      OopMapBlock* new_map = new_class->start_of_nonstatic_oop_maps();
      new_map->set_offset(old_map->offset());
      new_map->set_count(old_map->count());
    }

    if (new_class->reference_type() != REF_NONE) {
      assert(new_class->start_of_nonstatic_oop_maps()->offset() == the_class->start_of_nonstatic_oop_maps()->offset(), "oops map offset must be same");
    }

    int redefinition_flags = Klass::NoRedefinition;
    if (not_changed) {
      redefinition_flags = Klass::NoRedefinition;
    } else {
      redefinition_flags = calculate_redefinition_flags(new_class);
      if (redefinition_flags >= Klass::RemoveSuperType) {
        return JVMTI_ERROR_UNSUPPORTED_REDEFINITION_HIERARCHY_CHANGED;
      }
    }

    if (new_class->super() != nullptr) {
      redefinition_flags = redefinition_flags | new_class->super()->redefinition_flags();
    }

    for (int j = 0; j < new_class->local_interfaces()->length(); j++) {
      redefinition_flags = redefinition_flags | (new_class->local_interfaces()->at(j))->redefinition_flags();
    }

    new_class->set_redefinition_flags(redefinition_flags);

    _max_redefinition_flags = _max_redefinition_flags | redefinition_flags;

    if ((redefinition_flags & Klass::ModifyInstances) != 0) {
       calculate_instance_update_information(new_class);
    }

    if (the_class == vmClasses::Object_klass()) {
      _object_klass_redefined = true;
    }

    log_debug(redefine, class, load)
      ("loaded name=%s (avail_mem=" UINT64_FORMAT "K)", the_class->external_name(), os::available_memory() >> 10);
  }

  return JVMTI_ERROR_NONE;
}

 // Calculated the difference between new and old class  (field change, method change, supertype change, ...).
int VM_EnhancedRedefineClasses::calculate_redefinition_flags(InstanceKlass* new_class) {
  int result = Klass::NoRedefinition;
  log_debug(redefine, class, load)("Comparing different class versions of class %s",new_class->name()->as_C_string());

  assert(new_class->old_version() != nullptr, "must have old version");
  InstanceKlass* the_class = InstanceKlass::cast(new_class->old_version());

  // Check whether class is in the error init state.
  if (the_class->is_in_error_state()) {
    // TBD #5057930: special error code is needed in 1.6
    //result = Klass::union_redefinition_level(result, Klass::Invalid);
  }

  int i;

  // Check superclasses
  assert(new_class->super() == nullptr || new_class->super()->new_version() == nullptr, "superclass must be of newest version");
  if (the_class->super() != new_class->super()) {
    // Super class changed
    Klass* cur_klass = the_class->super();
    while (cur_klass != nullptr) {
      if (!new_class->is_subclass_of(cur_klass->newest_version())) {
        log_info(redefine, class, load)("removed super class %s", cur_klass->name()->as_C_string());
        result = result | Klass::RemoveSuperType | Klass::ModifyInstances | Klass::ModifyClass;
      }
      cur_klass = cur_klass->super();
    }

    cur_klass = new_class->super();
    while (cur_klass != nullptr) {
      if (!the_class->is_subclass_of(cur_klass->is_redefining() ? cur_klass->old_version() : cur_klass)) {
        log_info(redefine, class, load)("added super class %s", cur_klass->name()->as_C_string());
        result = result | Klass::ModifyClass | Klass::ModifyInstances;
      }
      cur_klass = cur_klass->super();
    }
  }

  // Check interfaces

  // Interfaces removed?
  Array<InstanceKlass*>* old_interfaces = the_class->transitive_interfaces();
  for (i = 0; i < old_interfaces->length(); i++) {
    InstanceKlass* old_interface = InstanceKlass::cast(old_interfaces->at(i));
    if (!new_class->implements_interface_any_version(old_interface)) {
      result = result | Klass::RemoveSuperType | Klass::ModifyClass;
      log_info(redefine, class, load)("removed interface %s", old_interface->name()->as_C_string());
    }
  }

  // Interfaces added?
  Array<InstanceKlass*>* new_interfaces = new_class->transitive_interfaces();
  for (i = 0; i<new_interfaces->length(); i++) {
    if (!the_class->implements_interface_any_version(new_interfaces->at(i))) {
      result = result | Klass::ModifyClass;
      log_info(redefine, class, load)("added interface %s", new_interfaces->at(i)->name()->as_C_string());
    }
  }

  // Check whether class modifiers are the same.
  jushort old_flags = (jushort) the_class->access_flags().get_flags();
  jushort new_flags = (jushort) new_class->access_flags().get_flags();
  if (old_flags != new_flags) {
    // FIXME: Can this have any effects?
  }

  // Check if the number, names, types and order of fields declared in these classes
  // are the same.
  JavaFieldStream old_fs(the_class);
  JavaFieldStream new_fs(new_class);
  for (; !old_fs.done() && !new_fs.done(); old_fs.next(), new_fs.next()) {
    // access
    old_flags = old_fs.access_flags().as_short();
    new_flags = new_fs.access_flags().as_short();
    if ((old_flags ^ new_flags) & JVM_RECOGNIZED_FIELD_MODIFIERS) {
      // FIXME: can this have any effect?
    }
    // offset
    if (old_fs.offset() != new_fs.offset()) {
      result = result | Klass::ModifyInstances;
    }
    // name and signature
    Symbol* name_sym1 = the_class->constants()->symbol_at(old_fs.name_index());
    Symbol* sig_sym1 = the_class->constants()->symbol_at(old_fs.signature_index());
    Symbol* name_sym2 = new_class->constants()->symbol_at(new_fs.name_index());
    Symbol* sig_sym2 = new_class->constants()->symbol_at(new_fs.signature_index());
    if (name_sym1 != name_sym2 || sig_sym1 != sig_sym2) {
      result = result | Klass::ModifyInstances;
    }
  }

  // If both streams aren't done then we have a differing number of
  // fields.
  if (!old_fs.done() || !new_fs.done()) {
    result = result | Klass::ModifyInstances;
  }

  // Do a parallel walk through the old and new methods. Detect
  // cases where they match (exist in both), have been added in
  // the new methods, or have been deleted (exist only in the
  // old methods).  The class file parser places methods in order
  // by method name, but does not order overloaded methods by
  // signature.  In order to determine what fate befell the methods,
  // this code places the overloaded new methods that have matching
  // old methods in the same order as the old methods and places
  // new overloaded methods at the end of overloaded methods of
  // that name. The code for this order normalization is adapted
  // from the algorithm used in InstanceKlass::find_method().
  // Since we are swapping out of order entries as we find them,
  // we only have to search forward through the overloaded methods.
  // Methods which are added and have the same name as an existing
  // method (but different signature) will be put at the end of
  // the methods with that name, and the name mismatch code will
  // handle them.
  Array<Method*>* k_old_methods(the_class->methods());
  Array<Method*>* k_new_methods(new_class->methods());
  int n_old_methods = k_old_methods->length();
  int n_new_methods = k_new_methods->length();
  Thread* thread = Thread::current();

  int ni = 0;
  int oi = 0;
  while (true) {
    Method* k_old_method;
    Method* k_new_method;
    enum { matched, added, deleted, undetermined } method_was = undetermined;

    if (oi >= n_old_methods) {
      if (ni >= n_new_methods) {
        break; // we've looked at everything, done
      }
      // New method at the end
      k_new_method = k_new_methods->at(ni);
      method_was = added;
    } else if (ni >= n_new_methods) {
      // Old method, at the end, is deleted
      k_old_method = k_old_methods->at(oi);
      method_was = deleted;
    } else {
      // There are more methods in both the old and new lists
      k_old_method = k_old_methods->at(oi);
      k_new_method = k_new_methods->at(ni);
      if (k_old_method->name() != k_new_method->name()) {
        // Methods are sorted by method name, so a mismatch means added
        // or deleted
        if (k_old_method->name()->fast_compare(k_new_method->name()) > 0) {
          method_was = added;
        } else {
          method_was = deleted;
        }
      } else if (k_old_method->signature() == k_new_method->signature()) {
        // Both the name and signature match
        method_was = matched;
      } else {
        // The name matches, but the signature doesn't, which means we have to
        // search forward through the new overloaded methods.
        int nj;  // outside the loop for post-loop check
        for (nj = ni + 1; nj < n_new_methods; nj++) {
          Method* m = k_new_methods->at(nj);
          if (k_old_method->name() != m->name()) {
            // reached another method name so no more overloaded methods
            method_was = deleted;
            break;
          }
          if (k_old_method->signature() == m->signature()) {
            // found a match so swap the methods
            k_new_methods->at_put(ni, m);
            k_new_methods->at_put(nj, k_new_method);
            k_new_method = m;
            method_was = matched;
            break;
          }
        }

        if (nj >= n_new_methods) {
          // reached the end without a match; so method was deleted
          method_was = deleted;
        }
      }
    }

    switch (method_was) {
    case matched:
      // methods match, be sure modifiers do too
      old_flags = (jushort) k_old_method->access_flags().get_flags();
      new_flags = (jushort) k_new_method->access_flags().get_flags();
      if ((old_flags ^ new_flags) & ~(JVM_ACC_NATIVE)) {
        // TODO Can this have any effects? Probably yes on vtables?
        result = result | Klass::ModifyClass;
      }
      {
        u2 new_num = k_new_method->method_idnum();
        u2 old_num = k_old_method->method_idnum();
        if (new_num != old_num) {
        Method* idnum_owner = new_class->method_with_idnum(old_num);
          if (idnum_owner != nullptr) {
            // There is already a method assigned this idnum -- switch them
            // Take current and original idnum from the new_method
            idnum_owner->set_method_idnum(new_num);
            idnum_owner->set_orig_method_idnum(k_new_method->orig_method_idnum());
          }
          // Take current and original idnum from the old_method
          k_new_method->set_method_idnum(old_num);
          k_new_method->set_orig_method_idnum(k_old_method->orig_method_idnum());
          if (thread->has_pending_exception()) {
            return JVMTI_ERROR_OUT_OF_MEMORY;
          }
        }
      }
      log_trace(redefine, class, normalize)
        ("Method matched: new: %s [%d] == old: %s [%d]",
         k_new_method->name_and_sig_as_C_string(), ni, k_old_method->name_and_sig_as_C_string(), oi);
      // advance to next pair of methods
      ++oi;
      ++ni;
      break;
    case added:
      // method added, see if it is OK
      new_flags = (jushort) k_new_method->access_flags().get_flags();
      if ((new_flags & JVM_ACC_PRIVATE) == 0
           // hack: private should be treated as final, but alas
          || (new_flags & (JVM_ACC_FINAL|JVM_ACC_STATIC)) == 0
         ) {
        // new methods must be private
        result = result | Klass::ModifyClass;
      }
      {
        u2 num = new_class->next_method_idnum();
        if (num == ConstMethod::UNSET_IDNUM) {
          // cannot add any more methods
        result = result | Klass::ModifyClass;
        }
        u2 new_num = k_new_method->method_idnum();
        Method* idnum_owner = new_class->method_with_idnum(num);
        if (idnum_owner != nullptr) {
          // There is already a method assigned this idnum -- switch them
          // Take current and original idnum from the new_method
          idnum_owner->set_method_idnum(new_num);
          idnum_owner->set_orig_method_idnum(k_new_method->orig_method_idnum());
        }
        k_new_method->set_method_idnum(num);
        k_new_method->set_orig_method_idnum(num);
        if (thread->has_pending_exception()) {
          return JVMTI_ERROR_OUT_OF_MEMORY;
        }
      }
      log_trace(redefine, class, normalize)
        ("Method added: new: %s [%d]", k_new_method->name_and_sig_as_C_string(), ni);
      ++ni; // advance to next new method
      break;
    case deleted:
      // method deleted, see if it is OK
      old_flags = (jushort) k_old_method->access_flags().get_flags();
      if ((old_flags & JVM_ACC_PRIVATE) == 0
           // hack: private should be treated as final, but alas
          || (old_flags & (JVM_ACC_FINAL|JVM_ACC_STATIC)) == 0
         ) {
        // deleted methods must be private
        result = result | Klass::ModifyClass;
      }
      log_trace(redefine, class, normalize)
        ("Method deleted: old: %s [%d]", k_old_method->name_and_sig_as_C_string(), oi);
      ++oi; // advance to next old method
      break;
    default:
      ShouldNotReachHere();
    }
  }

  if (new_class->size() != new_class->old_version()->size()) {
    result |= Klass::ModifyClassSize;
  }

  if (new_class->size_helper() != (InstanceKlass::cast((new_class->old_version()))->size_helper())) {
    result |= Klass::ModifyInstanceSize;
  }

  // TODO Check method bodies to be able to return NoChange?
  return result;
}


// Searches for the class bytecode of the given class and returns it as a byte array.
//  - the_class definition of a class, either existing class or new_class
//  - class_bytes - if the class is redefined, it contains new class definition, otherwise just original class bytecode.
//  - class_byte_count - size of class_bytes
//  - not_changed - new_class not available or same as current class
jvmtiError VM_EnhancedRedefineClasses::find_class_bytes(InstanceKlass* the_class, const unsigned char **class_bytes, jint *class_byte_count, jboolean *not_changed) {

  *not_changed = false;

  // Search for the index in the redefinition array that corresponds to the current class
  int i;
  for (i = 0; i < _class_count; i++) {
    if (the_class == get_ik(_class_defs[i].klass))
      break;
  }

  if (i == _class_count) {
    *not_changed = true;

    // Redefine with same bytecodes. This is a class that is only indirectly affected by redefinition,
    // so the user did not specify a different bytecode for that class.
    if (the_class->get_cached_class_file_bytes() == nullptr) {
      // Not cached, we need to reconstitute the class file from the
      // VM representation. We don't attach the reconstituted class
      // bytes to the InstanceKlass here because they have not been
      // validated, and we're not at a safepoint.
      JvmtiClassFileReconstituter reconstituter(the_class);
      if (reconstituter.get_error() != JVMTI_ERROR_NONE) {
        return reconstituter.get_error();
      }

      *class_byte_count = (jint)reconstituter.class_file_size();
      *class_bytes      = (unsigned char*) reconstituter.class_file_bytes();
    } else {
      // it is cached, get it from the cache
      *class_byte_count = the_class->get_cached_class_file_len();
      *class_bytes      = the_class->get_cached_class_file_bytes();
    }
  } else {
    // Redefine with bytecodes at index j
    *class_bytes = _class_defs[i].class_bytes;
    *class_byte_count = _class_defs[i].class_byte_count;
  }

  return JVMTI_ERROR_NONE;
}

// Calculate difference between non static fields of old and new class and store the info into new class:
//     instanceKlass->store_update_information
//     instanceKlass->copy_backwards
void VM_EnhancedRedefineClasses::calculate_instance_update_information(Klass* new_version) {

  class CalculateFieldUpdates : public FieldClosure {

  private:
    InstanceKlass* _old_ik;
    GrowableArray<int> _update_info;
    int _position;
    bool _copy_backwards;

  public:

    bool does_copy_backwards() {
      return _copy_backwards;
    }

    CalculateFieldUpdates(InstanceKlass* old_ik) :
        _old_ik(old_ik), _position(instanceOopDesc::base_offset_in_bytes()), _copy_backwards(false) {
      _update_info.append(_position);
      _update_info.append(0);
    }

    GrowableArray<int> &finish() {
      _update_info.append(0);
      return _update_info;
    }

    void do_field(fieldDescriptor* fd) {
      int alignment = fd->offset() - _position;
      if (alignment > 0) {
        // This field was aligned, so we need to make sure that we fill the gap
        fill(alignment);
      } else if (alignment < 0) {
        assert(false, "Fields must be sorted by offset!");
      }

      assert(_position == fd->offset(), "must be correct offset!");

      InstanceKlass* holder = fd->field_holder();
      if (fd->index() < holder->java_fields_count()) {
        fieldDescriptor old_fd;
        if (_old_ik->find_field(fd->name(), fd->signature(), false, &old_fd) != nullptr) {
          // Found field in the old class, copy
          copy(old_fd.offset(), type2aelembytes(fd->field_type()));

          if (old_fd.offset() < fd->offset()) {
            _copy_backwards = true;
          }

          // Transfer special flags
          fd->set_is_field_modification_watched(old_fd.is_field_modification_watched());
          fd->set_is_field_access_watched(old_fd.is_field_access_watched());
        } else {
          // New field, fill
          fill(type2aelembytes(fd->field_type()));
        }
      } else {
        FieldInfo internal_field = holder->field(fd->index());
        InstanceKlass* old_klass = InstanceKlass::cast(holder->old_version());
        int java_fields_count = old_klass->java_fields_count();
        int num_injected;
        const InjectedField* const injected = JavaClasses::get_injected(old_klass->name(), &num_injected);
        for (int i = java_fields_count; i < java_fields_count+num_injected; i++) {
          FieldInfo old_field = old_klass->field(i);
          if (old_field.field_flags().is_injected() &&
              internal_field.field_flags().is_injected() &&
              old_field.lookup_symbol(old_field.name_index()) == internal_field.lookup_symbol(internal_field.name_index())) {
            copy(old_field.offset(), type2aelembytes(Signature::basic_type(internal_field.signature_injected_dcevm())));
            if (old_field.offset() < internal_field.offset()) {
              _copy_backwards = true;
            }
            break;
          }
        }
      }
   }

  private:
    void fill(int size) {
      if (_update_info.length() > 0 && _update_info.at(_update_info.length() - 1) < 0) {
        (*_update_info.adr_at(_update_info.length() - 1)) -= size;
      } else {
        _update_info.append(-size);
      }
      _position += size;
    }

    void copy(int offset, int size) {
      int prev_end = -1;
      if (_update_info.length() > 0 && _update_info.at(_update_info.length() - 1) > 0) {
        prev_end = _update_info.at(_update_info.length() - 2) + _update_info.at(_update_info.length() - 1);
      }

      if (prev_end == offset) {
        (*_update_info.adr_at(_update_info.length() - 2)) += size;
      } else {
        _update_info.append(size);
        _update_info.append(offset);
      }

      _position += size;
    }
  };

  InstanceKlass* ik = InstanceKlass::cast(new_version);
  InstanceKlass* old_ik = InstanceKlass::cast(new_version->old_version());

  //
  CalculateFieldUpdates cl(old_ik);
  ik->do_nonstatic_fields_dcevm(&cl);

  GrowableArray<int> result = cl.finish();
  ik->store_update_information(result);
  ik->set_copying_backwards(cl.does_copy_backwards());
  if (log_is_enabled(Trace, redefine, class, obsolete, metadata)) {
    log_trace(redefine, class, obsolete, metadata)("Instance update information for %s:", new_version->name()->as_C_string());
    if (cl.does_copy_backwards()) {
      log_trace(redefine, class, obsolete, metadata)("\tDoes copy backwards!");
    }
    for (int i=0; i<result.length(); i++) {
      int curNum = result.at(i);
      if (curNum < 0) {
        log_trace(redefine, class, obsolete, metadata)("\t%d CLEAN", curNum);
      } else if (curNum > 0) {
        log_trace(redefine, class, obsolete, metadata)("\t%d COPY from %d", curNum, result.at(i + 1));
        i++;
      } else {
        log_trace(redefine, class, obsolete, metadata)("\tEND");
      }
    }
  }
}

// Rollback all changes - clear new classes from the system dictionary, return old classes to directory, free memory.
void VM_EnhancedRedefineClasses::rollback() {
  log_info(redefine, class, load)("Rolling back redefinition, result=%d", _res);
  ClassLoaderDataGraph_lock->lock();
  ClassLoaderDataGraph::rollback_redefinition();
  ClassLoaderDataGraph_lock->unlock();

  for (int i = 0; i < _new_classes->length(); i++) {
    SystemDictionary::remove_from_hierarchy(_new_classes->at(i));
  }

  for (int i = 0; i < _new_classes->length(); i++) {
    InstanceKlass* new_class = _new_classes->at(i);
    new_class->set_redefining(false);
    new_class->old_version()->set_new_version(nullptr);
    new_class->set_old_version(nullptr);
  }
  _new_classes->clear();
}


// Rewrite faster byte-codes back to their slower equivalent. Undoes rewriting happening in templateTable_xxx.cpp
// The reason is that once we zero cpool caches, we need to re-resolve all entries again. Faster bytecodes do not
// do that, they assume that cache entry is resolved already.
void VM_EnhancedRedefineClasses::unpatch_bytecode(Method* method) {
  RawBytecodeStream bcs(methodHandle(Thread::current(), method));
  Bytecodes::Code code;
  Bytecodes::Code java_code;
  while (!bcs.is_last_bytecode()) {
    code = bcs.raw_next();

    // dcevm : workaround check _illegal in case of lambda methods etc.
    // TODO: skip lambda/intrinsic before while loop?  (method()->is_method_handle_intrinsic() || method()->is_compiled_lambda_form())
    if (code == Bytecodes::_illegal) {
      return;
    }

    address bcp = bcs.bcp();

    if (code == Bytecodes::_breakpoint) {
      int bci = method->bci_from(bcp);
      code = method->orig_bytecode_at(bci, true);
      if (code != Bytecodes::_shouldnotreachhere) {
        java_code = Bytecodes::java_code(code);
        if (code != java_code &&
             (java_code == Bytecodes::_getfield ||
              java_code == Bytecodes::_putfield ||
              java_code == Bytecodes::_aload_0)) {
          // Let breakpoint table handling unpatch bytecode
          method->set_orig_bytecode_at(bci, java_code);
        }
      }
    } else {
      java_code = Bytecodes::java_code(code);
      if (code != java_code &&
           (java_code == Bytecodes::_getfield ||
            java_code == Bytecodes::_putfield ||
            java_code == Bytecodes::_aload_0)) {
        *bcp = java_code;
      }
    }

    // Additionally, we need to unpatch bytecode at bcp+1 for fast_xaccess (which would be fast field access)
    if (code == Bytecodes::_fast_iaccess_0 || code == Bytecodes::_fast_aaccess_0 || code == Bytecodes::_fast_faccess_0) {
      Bytecodes::Code code2 = Bytecodes::code_or_bp_at(bcp + 1);
      assert(code2 == Bytecodes::_fast_igetfield ||
             code2 == Bytecodes::_fast_agetfield ||
             code2 == Bytecodes::_fast_fgetfield, "");
        *(bcp + 1) = Bytecodes::java_code(code2);
      }
    }
  }

// Unevolving classes may point to old methods directly
// from their constant pool caches, itables, and/or vtables. We
// use the SystemDictionary::classes_do() facility and this helper
// to fix up these pointers. Additional field offsets and vtable indices
// in the constant pool cache entries are fixed.
//
// Note: We currently don't support updating the vtable in
// arrayKlassOops. See Open Issues in jvmtiRedefineClasses.hpp.
void VM_EnhancedRedefineClasses::ClearCpoolCacheAndUnpatch::do_klass(Klass* k) {
  if (!k->is_instance_klass()) {
    return;
  }

  HandleMark hm(_thread);
  InstanceKlass *ik = InstanceKlass::cast(k);

  constantPoolHandle other_cp = constantPoolHandle(_thread, ik->constants());

  // Update host klass of anonymous classes (for example, produced by lambdas) to the newest version.
  /*
  if (ik->is_unsafe_anonymous() && ik->unsafe_anonymous_host()->new_version() != nullptr) {
    ik->set_unsafe_anonymous_host(InstanceKlass::cast(ik->unsafe_anonymous_host()->newest_version()));
  }
  */

  // FIXME: check new nest_host for hidden

  // Update implementor if there is only one, in this case implementor() can reference old class
  if (ik->is_interface()) {
    Klass* implKlass = ik->implementor();
    if (implKlass != nullptr && implKlass != ik && implKlass->new_version() != nullptr) {
      InstanceKlass* newest_impl = InstanceKlass::cast(implKlass->newest_version());
      ik->init_implementor_from_redefine();
      if (newest_impl->implements_interface(ik)) {
        ik->add_implementor(newest_impl);
      }
    }
  }

  for (int i = 0; i < other_cp->length(); i++) {
    if (other_cp->tag_at(i).is_klass()) {
      Klass* klass = other_cp->resolved_klass_at(i);
      if (klass->new_version() != nullptr) {
        // Constant pool entry points to redefined class -- update to the new version
        other_cp->klass_at_put(i, klass->newest_version());
      }
      assert(other_cp->resolved_klass_at(i)->new_version() == nullptr, "Must be new klass!");
    }
  }

  // DCEVM - clear whole cache (instead special methods for class/method update in standard redefinition)
  ConstantPoolCache* cp_cache = other_cp->cache();
  if (cp_cache != nullptr) {
    cp_cache->clear_entries();
  }

  // If bytecode rewriting is enabled, we also need to unpatch bytecode to force resolution of zeroed entries
  if (RewriteBytecodes) {
    ik->methods_do(unpatch_bytecode);
  }
}

u8 VM_EnhancedRedefineClasses::next_id() {
  while (true) {
    u8 id = _id_counter;
    u8 next_id = id + 1;
    u8 result = Atomic::cmpxchg(&_id_counter, id, next_id);
    if (result == id) {
      return next_id;
    }
  }
}

// Clean method data for this class
void VM_EnhancedRedefineClasses::MethodDataCleaner::do_klass(Klass* k) {
  if (k->is_instance_klass()) {
    InstanceKlass *ik = InstanceKlass::cast(k);
    // Clean MethodData of this class's methods so they don't refer to
    // old methods that are no longer running.
    Array<Method*>* methods = ik->methods();
    int num_methods = methods->length();
    for (int index = 0; index < num_methods; ++index) {
      if (methods->at(index)->method_data() != nullptr) {
        methods->at(index)->method_data()->clean_weak_method_links();
      }
    }
  }
}


void VM_EnhancedRedefineClasses::update_jmethod_ids(Thread *current) {
  for (int j = 0; j < _matching_methods_length; ++j) {
    Method* old_method = _matching_old_methods[j];
    jmethodID jmid = old_method->find_jmethod_id_or_null();
    if (old_method->new_version() != nullptr && jmid == nullptr) {
       // (DCEVM) Have to create jmethodID in this case
       jmid = old_method->jmethod_id();
    }

    if (jmid != nullptr) {
      // There is a jmethodID, change it to point to the new method
      methodHandle new_method_h(current, _matching_new_methods[j]);

      if (old_method->new_version() == nullptr) {
        methodHandle old_method_h(current, _matching_old_methods[j]);
        jmethodID new_jmethod_id = Method::make_jmethod_id(old_method_h->method_holder()->class_loader_data(), old_method_h());
        bool result = InstanceKlass::cast(old_method_h->method_holder())->update_jmethod_id(old_method_h(), new_jmethod_id);
      } else {
        jmethodID mid = new_method_h->jmethod_id();
        bool result = InstanceKlass::cast(new_method_h->method_holder())->update_jmethod_id(new_method_h(), jmid);
      }

      Method::change_method_associated_with_jmethod_id(jmid, new_method_h());
      assert(Method::resolve_jmethod_id(jmid) == _matching_new_methods[j], "should be replaced");
    }
  }
}

// Set method as obsolete / old / deleted.
void VM_EnhancedRedefineClasses::check_methods_and_mark_as_obsolete() {
  for (int j = 0; j < _matching_methods_length; ++j/*, ++old_index*/) {
    Method* old_method = _matching_old_methods[j];
    Method* new_method = _matching_new_methods[j];

    if (MethodComparator::methods_EMCP(old_method, new_method)) {
      old_method->set_new_version(new_method);
      new_method->set_old_version(old_method);

      // Transfer breakpoints
      InstanceKlass *ik = InstanceKlass::cast(old_method->method_holder());
      for (BreakpointInfo* bp = ik->breakpoints(); bp != nullptr; bp = bp->next()) {
        if (bp->match(old_method)) {
          assert(bp->match(new_method), "if old method is method, then new method must match too");
          new_method->set_breakpoint(bp->bci());
        }
      }
    } else {
      // mark obsolete methods as such
      old_method->set_is_obsolete();

      // obsolete methods need a unique idnum so they become new entries in
      // the jmethodID cache in InstanceKlass
      if (old_method->method_idnum() != new_method->method_idnum()) {
        log_error(redefine, class, normalize)
          ("Method not matched: %d != %d  old: %s = new: %s",  old_method->method_idnum(), new_method->method_idnum(),
              old_method->name_and_sig_as_C_string(), new_method->name_and_sig_as_C_string());
        // assert(old_method->method_idnum() == new_method->method_idnum(), "must match");
      }
//      u2 num = InstanceKlass::cast(_the_class_oop)->next_method_idnum();
//      if (num != ConstMethod::UNSET_IDNUM) {
//        old_method->set_method_idnum(num);
//      }
    }
    old_method->set_is_old();
  }
  for (int i = 0; i < _deleted_methods_length; ++i) {
    Method* old_method = _deleted_methods[i];

    old_method->set_is_old();
    old_method->set_is_obsolete();
    // FIXME: this flag was added in dcevm10 since it is required in resolvedMethodTable.cpp
    old_method->set_is_deleted();
  }
}

// This internal class transfers the native function registration from old methods
// to new methods.  It is designed to handle both the simple case of unchanged
// native methods and the complex cases of native method prefixes being added and/or
// removed.
// It expects only to be used during the VM_EnhancedRedefineClasses op (a safepoint).
//
// This class is used after the new methods have been installed in "new_class".
//
// So, for example, the following must be handled.  Where 'm' is a method and
// a number followed by an underscore is a prefix.
//
//                                      Old Name    New Name
// Simple transfer to new method        m       ->  m
// Add prefix                           m       ->  1_m
// Remove prefix                        1_m     ->  m
// Simultaneous add of prefixes         m       ->  3_2_1_m
// Simultaneous removal of prefixes     3_2_1_m ->  m
// Simultaneous add and remove          1_m     ->  2_m
// Same, caused by prefix removal only  3_2_1_m ->  3_2_m
//
class TransferNativeFunctionRegistration {
 private:
  InstanceKlass* new_class;
  int prefix_count;
  char** prefixes;

  // Recursively search the binary tree of possibly prefixed method names.
  // Iteration could be used if all agents were well behaved. Full tree walk is
  // more resilient to agents not cleaning up intermediate methods.
  // Branch at each depth in the binary tree is:
  //    (1) without the prefix.
  //    (2) with the prefix.
  // where 'prefix' is the prefix at that 'depth' (first prefix, second prefix,...)
  Method* search_prefix_name_space(int depth, char* name_str, size_t name_len,
                                     Symbol* signature) {
    TempNewSymbol name_symbol = SymbolTable::probe(name_str, (int)name_len);
    if (name_symbol != nullptr) {
      Method* method = new_class->lookup_method(name_symbol, signature);
      if (method != nullptr) {
        // Even if prefixed, intermediate methods must exist.
        if (method->is_native()) {
          // Wahoo, we found a (possibly prefixed) version of the method, return it.
          return method;
        }
        if (depth < prefix_count) {
          // Try applying further prefixes (other than this one).
          method = search_prefix_name_space(depth+1, name_str, name_len, signature);
          if (method != nullptr) {
            return method; // found
          }

          // Try adding this prefix to the method name and see if it matches
          // another method name.
          char* prefix = prefixes[depth];
          size_t prefix_len = strlen(prefix);
          size_t trial_len = name_len + prefix_len;
          char* trial_name_str = NEW_RESOURCE_ARRAY(char, trial_len + 1);
          strcpy(trial_name_str, prefix);
          strcat(trial_name_str, name_str);
          method = search_prefix_name_space(depth+1, trial_name_str, trial_len,
                                            signature);
          if (method != nullptr) {
            // If found along this branch, it was prefixed, mark as such
            method->set_is_prefixed_native();
            return method; // found
          }
        }
      }
    }
    return nullptr;  // This whole branch bore nothing
  }

  // Return the method name with old prefixes stripped away.
  char* method_name_without_prefixes(Method* method) {
    Symbol* name = method->name();
    char* name_str = name->as_utf8();

    // Old prefixing may be defunct, strip prefixes, if any.
    for (int i = prefix_count-1; i >= 0; i--) {
      char* prefix = prefixes[i];
      size_t prefix_len = strlen(prefix);
      if (strncmp(prefix, name_str, prefix_len) == 0) {
        name_str += prefix_len;
      }
    }
    return name_str;
  }

  // Strip any prefixes off the old native method, then try to find a
  // (possibly prefixed) new native that matches it.
  Method* strip_and_search_for_new_native(Method* method) {
    ResourceMark rm;
    char* name_str = method_name_without_prefixes(method);
    return search_prefix_name_space(0, name_str, strlen(name_str),
                                    method->signature());
  }

 public:

  // Construct a native method transfer processor for this class.
  TransferNativeFunctionRegistration(InstanceKlass* _new_class) {
    assert(SafepointSynchronize::is_at_safepoint(), "sanity check");

    new_class = _new_class;
    prefixes = JvmtiExport::get_all_native_method_prefixes(&prefix_count);
  }

  // Attempt to transfer any of the old or deleted methods that are native
  void transfer_registrations(Method** old_methods, int methods_length) {
    for (int j = 0; j < methods_length; j++) {
      Method* old_method = old_methods[j];

      if (old_method->is_native() && old_method->has_native_function()) {
        Method* new_method = strip_and_search_for_new_native(old_method);
        if (new_method != nullptr) {
          // Actually set the native function in the new method.
          // Redefine does not send events (except CFLH), certainly not this
          // behind the scenes re-registration.
          new_method->set_native_function(old_method->native_function(),
                              !Method::native_bind_event_is_interesting);
        }
      }
    }
  }
};

// Don't lose the association between a native method and its JNI function.
void VM_EnhancedRedefineClasses::transfer_old_native_function_registrations(InstanceKlass* new_class) {
  TransferNativeFunctionRegistration transfer(new_class);
  transfer.transfer_registrations(_deleted_methods, _deleted_methods_length);
  transfer.transfer_registrations(_matching_old_methods, _matching_methods_length);
}

// DCEVM - it always deoptimizes everything! (because it is very difficult to find only correct dependencies)
// Deoptimize all compiled code that depends on this class.
//
// If the can_redefine_classes capability is obtained in the onload
// phase then the compiler has recorded all dependencies from startup.
// In that case we need only deoptimize and throw away all compiled code
// that depends on the class.
//
// If can_redefine_classes is obtained sometime after the onload
// phase then the dependency information may be incomplete. In that case
// the first call to RedefineClasses causes all compiled code to be
// thrown away. As can_redefine_classes has been obtained then
// all future compilations will record dependencies so second and
// subsequent calls to RedefineClasses need only throw away code
// that depends on the class.
//
void VM_EnhancedRedefineClasses::flush_dependent_code() {
  assert_locked_or_safepoint(Compile_lock);

  DeoptimizationScope deopt_scope;

  // All dependencies have been recorded from startup or this is a second or
  // subsequent use of RedefineClasses
  // FIXME: for now, deoptimize all!
  if (0 && JvmtiExport::all_dependencies_are_recorded()) {
    CodeCache::mark_dependents_for_evol_deoptimization(&deopt_scope);
    log_debug(redefine, class, nmethod)("Marked dependent nmethods for deopt");
  } else {
    CodeCache::mark_all_nmethods_for_evol_deoptimization(&deopt_scope);
  }

  deopt_scope.deoptimize_marked();

  // From now on we know that the dependency information is complete
  JvmtiExport::set_all_dependencies_are_recorded(true);
}

//  Compare _old_methods and _new_methods arrays and store the result into
//  _matching_old_methods, _matching_new_methods, _added_methods, _deleted_methods
//  Setup _old_methods and _new_methods before the call - it should be called for one class only!
void VM_EnhancedRedefineClasses::compute_added_deleted_matching_methods() {
  Method* old_method;
  Method* new_method;

  _matching_old_methods = NEW_RESOURCE_ARRAY(Method*, _old_methods->length());
  _matching_new_methods = NEW_RESOURCE_ARRAY(Method*, _old_methods->length());
  _added_methods        = NEW_RESOURCE_ARRAY(Method*, _new_methods->length());
  _deleted_methods      = NEW_RESOURCE_ARRAY(Method*, _old_methods->length());

  _matching_methods_length = 0;
  _deleted_methods_length  = 0;
  _added_methods_length    = 0;

  int nj = 0;
  int oj = 0;
  while (true) {
    if (oj >= _old_methods->length()) {
      if (nj >= _new_methods->length()) {
        break; // we've looked at everything, done
      }
      // New method at the end
      new_method = _new_methods->at(nj);
      _added_methods[_added_methods_length++] = new_method;
      ++nj;
    } else if (nj >= _new_methods->length()) {
      // Old method, at the end, is deleted
      old_method = _old_methods->at(oj);
      _deleted_methods[_deleted_methods_length++] = old_method;
      ++oj;
    } else {
      old_method = _old_methods->at(oj);
      new_method = _new_methods->at(nj);
      if (old_method->name() == new_method->name()) {
        if (old_method->signature() == new_method->signature()) {
          _matching_old_methods[_matching_methods_length  ] = old_method;
          _matching_new_methods[_matching_methods_length++] = new_method;
          ++nj;
          ++oj;
        } else {
          // added overloaded have already been moved to the end,
          // so this is a deleted overloaded method
          _deleted_methods[_deleted_methods_length++] = old_method;
          ++oj;
        }
      } else { // names don't match
        if (old_method->name()->fast_compare(new_method->name()) > 0) {
          // new method
          _added_methods[_added_methods_length++] = new_method;
          ++nj;
        } else {
          // deleted method
          _deleted_methods[_deleted_methods_length++] = old_method;
          ++oj;
        }
      }
    }
  }
  assert(_matching_methods_length + _deleted_methods_length == _old_methods->length(), "sanity");
  assert(_matching_methods_length + _added_methods_length == _new_methods->length(), "sanity");
}

// Install the redefinition of a class:
//    - house keeping (flushing breakpoints and caches, deoptimizing
//      dependent compiled code)
//    - replacing parts in the_class with parts from new_class
//    - adding a weak reference to track the obsolete but interesting
//      parts of the_class
//    - adjusting constant pool caches and vtables in other classes
//      that refer to methods in the_class. These adjustments use the
//      ClassLoaderDataGraph::classes_do() facility which only allows
//      a helper method to be specified. The interesting parameters
//      that we would like to pass to the helper method are saved in
//      static global fields in the VM operation.
void VM_EnhancedRedefineClasses::redefine_single_class(Thread *current, InstanceKlass* new_class) {

  HandleMark hm(current);   // make sure handles from this call are freed

  InstanceKlass* the_class = InstanceKlass::cast(new_class->old_version());
  assert(the_class != nullptr, "must have old version");

  // Remove all breakpoints in methods of this class
  JvmtiBreakpoints& jvmti_breakpoints = JvmtiCurrentBreakpoints::get_jvmti_breakpoints();
  jvmti_breakpoints.clearall_in_class_at_safepoint(the_class);

  _old_methods = the_class->methods();
  _new_methods = new_class->methods();
  _the_class_oop = the_class;
  compute_added_deleted_matching_methods();

  // track number of methods that are EMCP for add_previous_version() call below
  check_methods_and_mark_as_obsolete();
  update_jmethod_ids(current);

  _any_class_has_resolved_methods = the_class->has_resolved_methods() || _any_class_has_resolved_methods;

  transfer_old_native_function_registrations(new_class);


  // JSR-292 support
  /* FIXME: j10 dropped support for it?
  MemberNameTable* mnt = the_class->member_names();
  assert(new_class->member_names() == nullptr, "");
  if (mnt != nullptr) {
    new_class->set_member_names(mnt);
    the_class->set_member_names(nullptr);

    // FIXME: adjust_method_entries is used in standard hotswap JDK9
    // bool trace_name_printed = false;
    // mnt->adjust_method_entries(new_class(), &trace_name_printed);
  }
  */

  {
    ResourceMark rm(current);
    // increment the classRedefinedCount field in the_class and in any
    // direct and indirect subclasses of the_class
    increment_class_counter(current, new_class);
    log_info(redefine, class, load)
      ("redefined name=%s, count=%d (avail_mem=" UINT64_FORMAT "K)",
       new_class->external_name(), java_lang_Class::classRedefinedCount(new_class->java_mirror()), os::available_memory() >> 10);
    Events::log_redefinition(current, "redefined class name=%s, count=%d",
                             new_class->external_name(),
                             java_lang_Class::classRedefinedCount(new_class->java_mirror()));
  }
} // end redefine_single_class()


// Increment the classRedefinedCount field in the specific InstanceKlass
// and in all direct and indirect subclasses.
void VM_EnhancedRedefineClasses::increment_class_counter(Thread *current, InstanceKlass *ik) {
  oop class_mirror = ik->old_version()->java_mirror();
  Klass* class_oop = java_lang_Class::as_Klass(class_mirror);
  int new_count = java_lang_Class::classRedefinedCount(class_mirror) + 1;
  java_lang_Class::set_classRedefinedCount(ik->java_mirror(), new_count);
}

void VM_EnhancedRedefineClasses::check_class(InstanceKlass* ik) {
  if (ik->is_instance_klass() && ik->old_version() != nullptr) {
    HandleMark hm(Thread::current());

    assert(ik->new_version() == nullptr, "must be latest version in system dictionary");

    if (ik->vtable_length() > 0) {
      ResourceMark rm(Thread::current());
      assert(ik->vtable().check_no_old_or_obsolete_entries(), "old method found");
      ik->vtable().verify(tty, true);
    }
  }
}

// Logging of all methods (old, new, changed, ...)
void VM_EnhancedRedefineClasses::dump_methods() {
  int j;
  log_trace(redefine, class, dump)("_old_methods --");
  for (j = 0; j < _old_methods->length(); ++j) {
    LogStreamHandle(Trace, redefine, class, dump) log_stream;
    Method* m = _old_methods->at(j);
    log_stream.print("%4d  (%5d)  ", j, m->vtable_index());
    m->access_flags().print_on(&log_stream);
    log_stream.print(" --  ");
    m->print_name(&log_stream);
    log_stream.cr();
  }
  log_trace(redefine, class, dump)("_new_methods --");
  for (j = 0; j < _new_methods->length(); ++j) {
    LogStreamHandle(Trace, redefine, class, dump) log_stream;
    Method* m = _new_methods->at(j);
    log_stream.print("%4d  (%5d)  ", j, m->vtable_index());
    m->access_flags().print_on(&log_stream);
    log_stream.print(" --  ");
    m->print_name(&log_stream);
    log_stream.cr();
  }
  log_trace(redefine, class, dump)("_matching_methods --");
  for (j = 0; j < _matching_methods_length; ++j) {
    LogStreamHandle(Trace, redefine, class, dump) log_stream;
    Method* m = _matching_old_methods[j];
    log_stream.print("%4d  (%5d)  ", j, m->vtable_index());
    m->access_flags().print_on(&log_stream);
    log_stream.print(" --  ");
    m->print_name();
    log_stream.cr();

    m = _matching_new_methods[j];
    log_stream.print("      (%5d)  ", m->vtable_index());
    m->access_flags().print_on(&log_stream);
    log_stream.cr();
  }
  log_trace(redefine, class, dump)("_deleted_methods --");
  for (j = 0; j < _deleted_methods_length; ++j) {
    LogStreamHandle(Trace, redefine, class, dump) log_stream;
    Method* m = _deleted_methods[j];
    log_stream.print("%4d  (%5d)  ", j, m->vtable_index());
    m->access_flags().print_on(&log_stream);
    log_stream.print(" --  ");
    m->print_name(&log_stream);
    log_stream.cr();
  }
  log_trace(redefine, class, dump)("_added_methods --");
  for (j = 0; j < _added_methods_length; ++j) {
    LogStreamHandle(Trace, redefine, class, dump) log_stream;
    Method* m = _added_methods[j];
    log_stream.print("%4d  (%5d)  ", j, m->vtable_index());
    m->access_flags().print_on(&log_stream);
    log_stream.print(" --  ");
    m->print_name(&log_stream);
    log_stream.cr();
  }
}

// Helper class to traverse all loaded classes and figure out if the class is affected by redefinition.
class AffectedKlassClosure : public KlassClosure {
 private:
   GrowableArray<Klass*>* _affected_klasses;
 public:
  AffectedKlassClosure(GrowableArray<Klass*>* affected_klasses) : _affected_klasses(affected_klasses) {}

  void do_klass(Klass* klass) {
    assert(!_affected_klasses->contains(klass), "must not occur more than once!");

    // allow only loaded classes
    if (!klass->is_instance_klass() || !InstanceKlass::cast(klass)->is_loaded()) {
      return;
    }

    if (klass->new_version() != nullptr && !klass->new_version()->is_redefining()) {
      return;
    }

    if (klass->is_redefining()) {
      return;
    }

    if (klass->check_redefinition_flag(Klass::MarkedAsAffected)) {
      _affected_klasses->append(klass);
      return;
    }

    int super_depth = klass->super_depth();
    int idx;
    for (idx = 0; idx < super_depth; idx++) {
      Klass* primary = klass->primary_super_of_depth(idx);
      if (primary == nullptr) {
        break;
      }
      if (primary->check_redefinition_flag(Klass::MarkedAsAffected)) {
        log_trace(redefine, class, load)("found affected class: %s", klass->name()->as_C_string());
        klass->set_redefinition_flag(Klass::MarkedAsAffected);
        _affected_klasses->append(klass);
        return;
      }
    }

    int secondary_length = klass->secondary_supers()->length();
    for (idx = 0; idx < secondary_length; idx++) {
      Klass* secondary = klass->secondary_supers()->at(idx);
      if (secondary->check_redefinition_flag(Klass::MarkedAsAffected)) {
        log_trace(redefine, class, load)("found affected class: %s", klass->name()->as_C_string());
        klass->set_redefinition_flag(Klass::MarkedAsAffected);
        _affected_klasses->append(klass);
        return;
      }
    }
  }
};

// Find all affected classes by current redefinition (either because of redefine, class hierarchy or interface change).
// Affected classes are stored in _affected_klasses and parent classes always precedes child class.
jvmtiError VM_EnhancedRedefineClasses::find_sorted_affected_classes(bool do_initial_mark, GrowableArray<Klass*>* prev_affected_klasses, TRAPS) {
  if (do_initial_mark) {
    for (int i = 0; i < _class_count; i++) {
      InstanceKlass* klass = get_ik(_class_defs[i].klass);
      klass->set_redefinition_flag(Klass::MarkedAsAffected | Klass::PrimaryRedefine);
      assert(klass->new_version() == nullptr, "must be new class");
      log_trace(redefine, class, load)("marking class as being redefined: %s", klass->name()->as_C_string());
    }
  } else {
    for (int i = 0; i < prev_affected_klasses->length(); i++) {
      if (prev_affected_klasses->at(i) != nullptr) {
        InstanceKlass *klass = InstanceKlass::cast(prev_affected_klasses->at(i));
        klass->set_redefinition_flag(Klass::MarkedAsAffected);
      }
    }
  }

  // Find classes not directly redefined, but affected by a redefinition (because one of its supertypes is redefined)
  AffectedKlassClosure closure(_affected_klasses);
  // Updated in j10, from original SystemDictionary::classes_do

  {
    MutexLocker mcld(ClassLoaderDataGraph_lock);
    // Hidden classes are not in SystemDictionary, so we have to iterate ClassLoaderDataGraph
    ClassLoaderDataGraph::classes_do(&closure);
  }

  log_trace(redefine, class, load)("%d classes affected", _affected_klasses->length());

  // Sort the affected klasses such that a supertype is always on a smaller array index than its subtype.
  jvmtiError result = do_topological_class_sorting(THREAD);

  if (log_is_enabled(Trace, redefine, class, load)) {
    log_trace(redefine, class, load)("redefine order:");
    for (int i = 0; i < _affected_klasses->length(); i++) {
      log_trace(redefine, class, load)("%s", _affected_klasses->at(i)->name()->as_C_string());
    }
  }
  return JVMTI_ERROR_NONE;
}

// Pairs of class dependencies (for topological sort)
struct KlassPair {
  const Klass* _left;
  const Klass* _right;

  KlassPair() { }
  KlassPair(const Klass* left, const Klass* right) : _left(left), _right(right) { }
};

static bool match_second(void* value, KlassPair elem) {
  return elem._right == value;
}

// For each class to be redefined parse the bytecode and figure out the superclass and all interfaces.
// First newly introduced classes (_class_defs) are scanned and then affected classed (_affected_klasses).
// Affected flag is cleared (clear_redefinition_flag(Klass::MarkedAsAffected))
// For each dependency create a KlassPair instance. Finally, affected classes (_affected_klasses) are sorted according to pairs.
// TODO - the class file is potentially parsed multiple times - introduce a cache?
jvmtiError VM_EnhancedRedefineClasses::do_topological_class_sorting(TRAPS) {
  ResourceMark mark(THREAD);

  // Collect dependencies
  GrowableArray<KlassPair> links;
  for (int i = 0; i < _class_count; i++) {
    InstanceKlass* klass = get_ik(_class_defs[i].klass);

    ClassFileStream st((u1*)_class_defs[i].class_bytes,
                           _class_defs[i].class_byte_count,
                           "__VM_EnhancedRedefineClasses__",
                           ClassFileStream::verify);

    Handle protection_domain(THREAD, klass->protection_domain());

    ClassLoadInfo cl_info(protection_domain);

    ClassFileParser parser(&st,
                           klass->name(),
                           klass->class_loader_data(),
                           &cl_info,
                           ClassFileParser::INTERNAL, // publicity level
                           true,
                           THREAD);

    const Klass* super_klass = parser.super_klass();
    if (super_klass != nullptr && _affected_klasses->contains((Klass*) super_klass)) {
      links.append(KlassPair(super_klass, klass));
    }

    Array<InstanceKlass*>* local_interfaces = parser.local_interfaces();
    for (int j = 0; j < local_interfaces->length(); j++) {
      Klass* iface = local_interfaces->at(j);
      if (iface != nullptr && _affected_klasses->contains(iface)) {
        links.append(KlassPair(iface, klass));
      }
    }

    assert(klass->check_redefinition_flag(Klass::MarkedAsAffected), "");
    klass->clear_redefinition_flag(Klass::MarkedAsAffected);
  }

  // Append dependencies based on current class definition
  for (int i = 0; i < _affected_klasses->length(); i++) {
    InstanceKlass* klass = InstanceKlass::cast(_affected_klasses->at(i));

    if (klass->check_redefinition_flag(Klass::MarkedAsAffected)) {
      klass->clear_redefinition_flag(Klass::MarkedAsAffected);
      Klass* super_klass = klass->super();
      if (_affected_klasses->contains(super_klass)) {
        links.append(KlassPair(super_klass, klass));
      }

      Array<InstanceKlass*>* local_interfaces = klass->local_interfaces();
      for (int j = 0; j < local_interfaces->length(); j++) {
        Klass* interfaceKlass = local_interfaces->at(j);
        if (_affected_klasses->contains(interfaceKlass)) {
          links.append(KlassPair(interfaceKlass, klass));
        }
      }
    }
  }

  for (int i = 0; i < _affected_klasses->length(); i++) {
    int j;
    for (j = _affected_klasses->length()-1; j >= i; j--) {
      // Search for node with no incoming edges
      // Search from the end of '_affected' since root classes are more likely to be found there than at the beginning.
      Klass* klass = _affected_klasses->at(j);
      int k = links.find(klass, match_second);
      if (k == -1) break;
    }
    if (j < i) {
      return JVMTI_ERROR_CIRCULAR_CLASS_DEFINITION;
    }
    // Remove all links from this node
    const Klass* klass = _affected_klasses->at(j);
    int k = 0;
    while (k < links.length()) {
      if (links.at(k)._left == klass) {
        links.delete_at(k);
      } else {
        k++;
      }
    }

    // Swap node
    Klass* tmp = _affected_klasses->at(j);
    _affected_klasses->at_put(j, _affected_klasses->at(i));
    _affected_klasses->at_put(i, tmp);
  }

  return JVMTI_ERROR_NONE;
}
