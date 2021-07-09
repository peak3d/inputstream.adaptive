// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "native_library.h"

#include <dlfcn.h>
#include <mach-o/getsect.h>

namespace base {

static NativeLibraryObjCStatus GetObjCStatusForImage(
    const void* function_pointer) {
  Dl_info info;
  if (!dladdr(function_pointer, &info))
    return OBJC_UNKNOWN;

  // See if the the image contains an "ObjC image info" segment. This method
  // of testing is used in _CFBundleGrokObjcImageInfoFromFile in
  // CF-744/CFBundle.c, around lines 2447-2474.
  //
  // In 32-bit images, ObjC can be recognized in __OBJC,__image_info, whereas
  // in 64-bit, the data is in __DATA,__objc_imageinfo.
#if __LP64__
  const section_64* section = getsectbynamefromheader_64(
      reinterpret_cast<const struct mach_header_64*>(info.dli_fbase),
      SEG_DATA, "__objc_imageinfo");
#else
  const section* section = getsectbynamefromheader(
      reinterpret_cast<const struct mach_header*>(info.dli_fbase),
      SEG_OBJC, "__image_info");
#endif
  return section == NULL ? OBJC_NOT_PRESENT : OBJC_PRESENT;
}

std::string NativeLibraryLoadError::ToString() const {
  return message;
}

// static
NativeLibrary LoadNativeLibrary(const std::string& library_path,
                                NativeLibraryLoadError* error) {
  // dlopen() etc. open the file off disk.
  std::string::size_type delim(library_path.find_last_of('.', library_path.length()));
  if ((delim != std::string::npos && library_path.substr(delim+1) == "dylib")) {
    void* dylib = dlopen(library_path.c_str(), RTLD_LAZY);
    if (!dylib) {
      if (error)
        error->message = dlerror();
      return NULL;
    }
    NativeLibrary native_lib = new NativeLibraryStruct();
    native_lib->type = DYNAMIC_LIB;
    native_lib->dylib = dylib;
    native_lib->objc_status = OBJC_UNKNOWN;
    return native_lib;
  }

  return NULL;
}

// static
void UnloadNativeLibrary(NativeLibrary library) {
  if (!library)
    return;
  if (library->objc_status == OBJC_NOT_PRESENT) {
    dlclose(library->dylib);
  }
  delete library;
}

// static
void* GetFunctionPointerFromNativeLibrary(NativeLibrary library,
                                          const char* name) {
  void* function_pointer = NULL;

  // Get the function pointer
  function_pointer = dlsym(library->dylib, name);

  // If this library hasn't been tested for having ObjC, use the function
  // pointer to look up the section information for the library.
  if (function_pointer && library->objc_status == OBJC_UNKNOWN)
    library->objc_status = GetObjCStatusForImage(function_pointer);

  return function_pointer;
}

// static
//string16 GetNativeLibraryName(const string16& name) {
//  return name + ASCIIToUTF16(".dylib");
//}

}  // namespace base
