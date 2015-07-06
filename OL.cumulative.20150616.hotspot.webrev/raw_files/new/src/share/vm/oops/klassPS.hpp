/*
 * Copyright (c) 2007, 2013, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_OOPS_KLASSPS_HPP
#define SHARE_VM_OOPS_KLASSPS_HPP

#include "utilities/macros.hpp"

// Macros that expand to Parallel Scavenge and Parallel Old GC related
// declarations

#if INCLUDE_ALL_GCS
#define PARALLEL_GC_DECLS                                              \
  virtual void oop_push_contents(PSPromotionManager* pm, oop obj);     \
  virtual void oop_follow_contents(ParCompactionManager* cm, oop obj); \
  virtual int oop_update_pointers(ParCompactionManager* cm, oop obj);

// Pure virtual version for klass.hpp
#define PARALLEL_GC_DECLS_PV                                               \
  virtual void oop_push_contents(PSPromotionManager* pm, oop obj) = 0;     \
  virtual void oop_follow_contents(ParCompactionManager* cm, oop obj) = 0; \
  virtual int oop_update_pointers(ParCompactionManager* cm, oop obj) = 0;
#else // INCLUDE_ALL_GCS
#define PARALLEL_GC_DECLS
#define PARALLEL_GC_DECLS_PV
#endif // INCLUDE_ALL_GCS

#endif // SHARE_VM_OOPS_KLASSPS_HPP
