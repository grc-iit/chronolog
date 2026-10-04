#include "../native/enum_names.h"
#include <napi.h>

Napi::Object Init(Napi::Env env, Napi::Object exports)
{
    exports.Set("status",
                Napi::Function::New(env,
                                    [](const Napi::CallbackInfo& info)
                                    {
                                        auto result = Napi::Object::New(info.Env());
                                        result.Set("code", binding::codeName(absl::StatusCode::kFailedPrecondition));
                                        result.Set("message", "new rejection");
                                        result.Set("rejection",
                                                   binding::rejectionName(static_cast<chronolog::AppendRejection>(14)));
                                        return result;
                                    }));
    exports.Set("capacity",
                Napi::Function::New(env,
                                    [](const Napi::CallbackInfo& info)
                                    {
                                        auto result = Napi::Object::New(info.Env());
                                        result.Set("code", binding::codeName(absl::StatusCode::kResourceExhausted));
                                        result.Set("message", "admission capacity");
                                        result.Set("rejection",
                                                   binding::rejectionName(chronolog::AppendRejection::Capacity));
                                        return result;
                                    }));
    return exports;
}
NODE_API_MODULE(chronolog_enum_test, Init)
