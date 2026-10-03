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
                                                   binding::rejectionName(static_cast<chronolog::AppendRejection>(13)));
                                        return result;
                                    }));
    return exports;
}
NODE_API_MODULE(chronolog_enum_test, Init)
