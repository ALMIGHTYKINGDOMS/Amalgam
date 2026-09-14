#pragma once

#include <jni.h>

namespace aml::bridge {

jint on_load(JavaVM* vm, void* reserved);
void on_unload(JavaVM* vm);

JavaVM* vm();

}  // namespace aml::bridge