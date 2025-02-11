#include <iostream>
#include <library/cpp/actors/core/actor.h>
#include <util/generic/ptr.h>
#include <library/cpp/actors/util/should_continue.h>

THolder<NActors::IActor> CreateSelfPingActor(const TDuration& latency);

std::shared_ptr<TProgramShouldContinue> GetProgramShouldContinue();

THolder<NActors::IActor> CreateTReadActor(const NActors::TActorId writeActor);

THolder<NActors::IActor> CreateTMaximumPrimeDevisorActor(const NActors::TActorIdentity readActor, const NActors::TActorId writeActor, int64_t value);

THolder<NActors::IActor> CreateTWriteActor();