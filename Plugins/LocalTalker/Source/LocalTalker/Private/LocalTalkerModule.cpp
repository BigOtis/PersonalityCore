#include "Modules/ModuleManager.h"
#include "LocalTalkerLlamaCache.h"

class FLocalTalkerModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override
    {
        // Ensure we release llama.cpp resources on shutdown (editor/game exit).
        FLocalTalkerLlamaCache::Get().Shutdown();
    }
};

IMPLEMENT_MODULE(FLocalTalkerModule, LocalTalker)
