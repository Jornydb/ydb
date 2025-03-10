#include "dq_compute_actor.h"
#include "dq_async_compute_actor.h"
#include "dq_compute_actor_async_input_helper.h"
#include "dq_task_runner_exec_ctx.h"

#include <ydb/library/yql/dq/actors/compute/dq_compute_actor_impl.h>

#include <ydb/library/yql/dq/common/dq_common.h>

#include <ydb/core/quoter/public/quoter.h>

namespace NYql {
namespace NDq {

constexpr TDuration MIN_QUOTED_CPU_TIME = TDuration::MilliSeconds(10);

using namespace NActors;

namespace {

bool IsDebugLogEnabled(const TActorSystem* actorSystem) {
    auto* settings = actorSystem->LoggerSettings();
    return settings && settings->Satisfies(NActors::NLog::EPriority::PRI_DEBUG, NKikimrServices::KQP_COMPUTE);
}

} // anonymous namespace

struct TComputeActorAsyncInputHelperForTaskRunnerActor : public TComputeActorAsyncInputHelper
{
public:
    TComputeActorAsyncInputHelperForTaskRunnerActor(
            const TString& logPrefix,
            ui64 index,
            NDqProto::EWatermarksMode watermarksMode,
            ui64& cookie,
            int& inflight
    )
        : TComputeActorAsyncInputHelper(logPrefix, index, watermarksMode)
        , TaskRunnerActor(nullptr)
        , Cookie(cookie)
        , Inflight(inflight)
        , FreeSpace(1)
        , PushStarted(false)
    {}

    i64 GetFreeSpace() const override
    {
        return FreeSpace;
    }

    void AsyncInputPush(NKikimr::NMiniKQL::TUnboxedValueBatch&& batch, i64 space, bool finished) override
    {
        Inflight++;
        PushStarted = true;
        Finished = finished;
        Y_ABORT_UNLESS(TaskRunnerActor);
        TaskRunnerActor->AsyncInputPush(Cookie++, Index, std::move(batch), space, finished);
    }

    NTaskRunnerActor::ITaskRunnerActor* TaskRunnerActor;
    ui64& Cookie;
    int& Inflight;
    i64 FreeSpace;
    bool PushStarted;
};

class TDqAsyncComputeActor : public TDqComputeActorBase<TDqAsyncComputeActor, TComputeActorAsyncInputHelperForTaskRunnerActor>
                           , public NTaskRunnerActor::ITaskRunnerActor::ICallbacks
{
    using TBase = TDqComputeActorBase<TDqAsyncComputeActor, TComputeActorAsyncInputHelperForTaskRunnerActor>;
public:
    static constexpr char ActorName[] = "DQ_COMPUTE_ACTOR";

    static constexpr bool HasAsyncTaskRunner = true;

    TDqAsyncComputeActor(const TActorId& executerId, const TTxId& txId, NDqProto::TDqTask* task,
        IDqAsyncIoFactory::TPtr asyncIoFactory,
        const NKikimr::NMiniKQL::IFunctionRegistry* functionRegistry,
        const TComputeRuntimeSettings& settings, const TComputeMemoryLimits& memoryLimits,
        const NTaskRunnerActor::ITaskRunnerActorFactory::TPtr& taskRunnerActorFactory,
        const ::NMonitoring::TDynamicCounterPtr& taskCounters,
        const TActorId& quoterServiceActorId,
        bool ownCounters)
        : TBase(executerId, txId, task, std::move(asyncIoFactory), functionRegistry, settings, memoryLimits, /* ownMemoryQuota = */ false, false, taskCounters)
        , TaskRunnerActorFactory(taskRunnerActorFactory)
        , ReadyToCheckpointFlag(false)
        , SentStatsRequest(false)
        , QuoterServiceActorId(quoterServiceActorId)
    {
        InitExtraMonCounters(taskCounters);
        if (ownCounters) {
            CA_LOG_D("TDqAsyncComputeActor, make stat");
            Stat = MakeHolder<NYql::TCounters>();
        }
    }

    void DoBootstrap() {
        const TActorSystem* actorSystem = TlsActivationContext->ActorSystem();

        TLogFunc logger;
        if (IsDebugLogEnabled(actorSystem)) {
            logger = [actorSystem, txId = GetTxId(), taskId = Task.GetId()] (const TString& message) {
                LOG_DEBUG_S(*actorSystem, NKikimrServices::KQP_COMPUTE, "TxId: " << txId
                    << ", task: " << taskId << ": " << message);
            };
        }

        NActors::IActor* actor;
        THashSet<ui32> inputWithDisabledCheckpointing;
        for (const auto&[idx, inputInfo]: InputChannelsMap) {
            if (inputInfo.CheckpointingMode == NDqProto::CHECKPOINTING_MODE_DISABLED) {
                inputWithDisabledCheckpointing.insert(idx);
            }
        }
        std::tie(TaskRunnerActor, actor) = TaskRunnerActorFactory->Create(
            this, TBase::GetAllocatorPtr(), GetTxId(), Task.GetId(), std::move(inputWithDisabledCheckpointing), InitMemoryQuota());
        TaskRunnerActorId = RegisterWithSameMailbox(actor);

        TDqTaskRunnerMemoryLimits limits;
        limits.ChannelBufferSize = MemoryLimits.ChannelBufferSize;
        limits.OutputChunkMaxSize = GetDqExecutionSettings().FlowControl.MaxOutputChunkSize;

        for (auto& [_, source]: SourcesMap) {
            source.TaskRunnerActor = TaskRunnerActor;
        }

        for (auto& [_, source]: InputTransformsMap) {
            source.TaskRunnerActor = TaskRunnerActor;
        }


        Become(&TDqAsyncComputeActor::StateFuncWrapper<&TDqAsyncComputeActor::StateFuncBody>);

        auto wakeup = [this]{ ContinueExecute(EResumeSource::CABootstrapWakeup); };
        std::shared_ptr<IDqTaskRunnerExecutionContext> execCtx = std::make_shared<TDqTaskRunnerExecutionContext>(TxId, std::move(wakeup));

        Send(TaskRunnerActorId,
            new NTaskRunnerActor::TEvTaskRunnerCreate(
                Task.GetSerializedTask(), limits, RuntimeSettings.StatsMode, execCtx));

        CA_LOG_D("Use CPU quota: " << UseCpuQuota() << ". Rate limiter resource: { \"" << Task.GetRateLimiter() << "\", \"" << Task.GetRateLimiterResource() << "\" }");
    }

    void FillExtraStats(NDqProto::TDqComputeActorStats* /* dst */, bool /* last */) {
    }

    void InitExtraMonCounters(const ::NMonitoring::TDynamicCounterPtr& taskCounters) {
        if (taskCounters && UseCpuQuota()) {
            CpuTimeGetQuotaLatency = taskCounters->GetHistogram("CpuTimeGetQuotaLatencyMs", NMonitoring::ExplicitHistogram({0, 1, 5, 10, 50, 100, 500, 1000}));
            CpuTimeQuotaWaitDelay = taskCounters->GetHistogram("CpuTimeQuotaWaitDelayMs", NMonitoring::ExplicitHistogram({0, 1, 5, 10, 50, 100, 500, 1000}));
            CpuTime = taskCounters->GetCounter("CpuTimeMs", true);
            CpuTime->Add(0);
        }
    }

    template<typename T>
    requires(std::is_base_of<TComputeActorAsyncInputHelperForTaskRunnerActor, T>::value)
    T CreateInputHelper(const TString& logPrefix,
        ui64 index,
        NDqProto::EWatermarksMode watermarksMode
    ) 
    {
        return T(logPrefix, index, watermarksMode, Cookie, ProcessSourcesState.Inflight);
    }

    const IDqAsyncInputBuffer* GetInputTransform(ui64 inputIdx, const TComputeActorAsyncInputHelperForTaskRunnerActor&) const {
        return TaskRunnerStats.GetInputTransform(inputIdx);
    }

private:
    STFUNC(StateFuncBody) {
        switch (ev->GetTypeRewrite()) {
            hFunc(NTaskRunnerActor::TEvTaskRunFinished, OnRunFinished);
            hFunc(NTaskRunnerActor::TEvSourceDataAck, OnSourceDataAck);
            hFunc(NTaskRunnerActor::TEvOutputChannelData, OnOutputChannelData);
            hFunc(NTaskRunnerActor::TEvTaskRunnerCreateFinished, OnTaskRunnerCreated);
            hFunc(NTaskRunnerActor::TEvInputChannelDataAck, OnInputChannelDataAck);
            hFunc(TEvDqCompute::TEvStateRequest, OnStateRequest);
            hFunc(NTaskRunnerActor::TEvStatistics, OnStatisticsResponse);
            hFunc(NTaskRunnerActor::TEvLoadTaskRunnerFromStateDone, OnTaskRunnerLoaded);
            hFunc(TEvDqCompute::TEvInjectCheckpoint, OnInjectCheckpoint);
            hFunc(TEvDqCompute::TEvRestoreFromCheckpoint, OnRestoreFromCheckpoint);
            hFunc(NKikimr::TEvQuota::TEvClearance, OnCpuQuotaGiven);
            default:
                TBase::BaseStateFuncBody(ev);
        };
    };

    void OnStateRequest(TEvDqCompute::TEvStateRequest::TPtr& ev) {
        CA_LOG_T("Got TEvStateRequest from actor " << ev->Sender << " PingCookie: " << ev->Cookie);
        if (!SentStatsRequest) {
            Send(TaskRunnerActorId, new NTaskRunnerActor::TEvStatistics(GetIds(SinksMap), GetIds(InputTransformsMap)));
            SentStatsRequest = true;
        }
        WaitingForStateResponse.push_back({ev->Sender, ev->Cookie});
    }

    void FillIssues(NYql::TIssues& issues) {
        auto applyToAllIssuesHolders = [this](auto& function){
            function(SourcesMap);
            function(InputTransformsMap);
            function(SinksMap);
            function(OutputTransformsMap);
        };

        uint64_t expectingSize = 0;
        auto addSize = [&expectingSize](auto& issuesHolder) {
            for (auto& [inputIndex, source]: issuesHolder) {
                expectingSize += source.IssuesBuffer.GetAllAddedIssuesCount();
            }
        };

        auto exhaustIssues = [&issues](auto& issuesHolder){
            for (auto& [inputIndex, source]: issuesHolder) {
            auto sourceIssues = source.IssuesBuffer.Dump();
                for (auto& issueInfo: sourceIssues) {
                    issues.AddIssues(issueInfo.Issues);
                }
            }
        };

        applyToAllIssuesHolders(addSize);
        issues.Reserve(expectingSize);
        applyToAllIssuesHolders(exhaustIssues);
    }

    void OnStatisticsResponse(NTaskRunnerActor::TEvStatistics::TPtr& ev) {
        SentStatsRequest = false;
        if (ev->Get()->Stats) {
            CA_LOG_T("update task runner stats");
            TaskRunnerStats = std::move(ev->Get()->Stats);
        }
        auto record = NDqProto::TEvComputeActorState();
        record.SetState(NDqProto::COMPUTE_STATE_EXECUTING);
        record.SetStatusCode(NYql::NDqProto::StatusIds::SUCCESS);
        record.SetTaskId(Task.GetId());
        NYql::TIssues issues;
        FillIssues(issues);

        IssuesToMessage(issues, record.MutableIssues());
        FillStats(record.MutableStats(), /* last */ false);
        for (const auto& [actorId, cookie] : WaitingForStateResponse) {
            auto state = MakeHolder<TEvDqCompute::TEvState>();
            state->Record = record;
            Send(actorId, std::move(state), NActors::IEventHandle::FlagTrackDelivery, cookie);
        }
        WaitingForStateResponse.clear();
    }

    const IDqAsyncOutputBuffer* GetSink(ui64 outputIdx, const TAsyncOutputInfoBase&) const override {
        return TaskRunnerStats.GetSink(outputIdx);
    }

    void DrainOutputChannel(TOutputChannelInfo& outputChannel) override {
        YQL_ENSURE(!outputChannel.Finished || Checkpoints);

        if (outputChannel.PopStarted) {
            return;
        }

        const bool wasFinished = outputChannel.Finished;
        auto channelId = outputChannel.ChannelId;

        const auto& peerState = Channels->GetOutputChannelInFlightState(channelId);

        CA_LOG_T("About to drain channelId: " << channelId
            << ", hasPeer: " << outputChannel.HasPeer
            << ", peerState:(" << peerState.DebugString() << ")"
//            << ", finished: " << outputChannel.Channel->IsFinished());
            );

        outputChannel.PopStarted = true;
        const bool hasFreeMemory = peerState.HasFreeMemory();
        UpdateBlocked(outputChannel, !hasFreeMemory);
        ProcessOutputsState.Inflight++;
        if (!hasFreeMemory) {
            CA_LOG_T("Can not drain channel because it is blocked by capacity. ChannelId: " << channelId
                << ", peerState:(" << peerState.DebugString() << ")"
            );
            auto ev = MakeHolder<NTaskRunnerActor::TEvOutputChannelData>(channelId);
            Y_ABORT_UNLESS(!ev->Finished);
            Send(SelfId(), std::move(ev));  // try again, ev.Finished == false
            return;
        }

        Send(TaskRunnerActorId, new NTaskRunnerActor::TEvOutputChannelDataRequest(channelId, wasFinished, peerState.GetFreeMemory()));
    }

    void DrainAsyncOutput(ui64 outputIndex, TAsyncOutputInfoBase& sinkInfo) override {
        if (sinkInfo.Finished && !Checkpoints) {
            return;
        }

        if (sinkInfo.PopStarted) {
            return;
        }

        Y_ABORT_UNLESS(sinkInfo.AsyncOutput);
        Y_ABORT_UNLESS(sinkInfo.Actor);

        const ui32 allowedOvercommit = AllowedChannelsOvercommit();
        const i64 sinkFreeSpaceBeforeSend = sinkInfo.AsyncOutput->GetFreeSpace();

        i64 toSend = sinkFreeSpaceBeforeSend + allowedOvercommit;
        CA_LOG_T("About to drain sink " << outputIndex
            << ". FreeSpace: " << sinkFreeSpaceBeforeSend
            << ", allowedOvercommit: " << allowedOvercommit
            << ", toSend: " << toSend
                 //<< ", finished: " << sinkInfo.Buffer->IsFinished());
            );

        sinkInfo.PopStarted = true;
        ProcessOutputsState.Inflight++;
        sinkInfo.FreeSpaceBeforeSend = sinkFreeSpaceBeforeSend;
        Send(TaskRunnerActorId, new NTaskRunnerActor::TEvSinkDataRequest(outputIndex, sinkFreeSpaceBeforeSend));
    }

    bool DoHandleChannelsAfterFinishImpl() override {
        Y_ABORT_UNLESS(Checkpoints);
        auto req = GetCheckpointRequest();
        if (!req.Defined()) {
            return true;  // handled channels syncronously
        }
        CA_LOG_D("DoHandleChannelsAfterFinishImpl");
        AskContinueRun(std::move(req), /* checkpointOnly = */ true);
        return false;
    }

    void PeerFinished(ui64 channelId) override {
        // no TaskRunner => no outputChannel.Channel, nothing to Finish
        CA_LOG_I("Peer finished, channel: " << channelId);
        TOutputChannelInfo* outputChannel = OutputChannelsMap.FindPtr(channelId);
        YQL_ENSURE(outputChannel, "task: " << Task.GetId() << ", output channelId: " << channelId);

        outputChannel->Finished = true;
        ProcessOutputsState.Inflight++;
        Send(TaskRunnerActorId, MakeHolder<NTaskRunnerActor::TEvOutputChannelDataRequest>(channelId, /* wasFinished = */ true, 0));  // finish channel
        DoExecute();
    }

    void TakeInputChannelData(TChannelDataOOB&& channelDataOOB, bool ack) override {
        CA_LOG_T("took input");
        NDqProto::TChannelData& channelData = channelDataOOB.Proto;
        TInputChannelInfo* inputChannel = InputChannelsMap.FindPtr(channelData.GetChannelId());
        YQL_ENSURE(inputChannel, "task: " << Task.GetId() << ", unknown input channelId: " << channelData.GetChannelId());

        auto finished = channelData.GetFinished();

        TMaybe<TInstant> watermark;
        if (channelData.HasWatermark()) {
            watermark = TInstant::MicroSeconds(channelData.GetWatermark().GetTimestampUs());

            const bool channelWatermarkChanged = WatermarksTracker.NotifyInChannelWatermarkReceived(
                inputChannel->ChannelId,
                *watermark
            );

            if (channelWatermarkChanged) {
                CA_LOG_T("Pause input channel " << channelData.GetChannelId() << " bacause of watermark " << *watermark);
                inputChannel->Pause(*watermark);
            }

            WatermarkTakeInputChannelDataRequests[*watermark]++;
        }

        TDqSerializedBatch batch;
        batch.Proto = std::move(*channelData.MutableData());
        batch.Payload = std::move(channelDataOOB.Payload);

        MetricsReporter.ReportInputChannelWatermark(
            channelData.GetChannelId(),
            batch.RowCount(),
            watermark);

        auto ev = MakeHolder<NTaskRunnerActor::TEvInputChannelData>(
            channelData.GetChannelId(),
            batch.RowCount() ? std::optional{std::move(batch)} : std::nullopt,
            finished,
            channelData.HasCheckpoint()
        );

        Send(TaskRunnerActorId, ev.Release(), 0, Cookie);

        if (channelData.HasCheckpoint()) {
            Y_ABORT_UNLESS(inputChannel->CheckpointingMode != NDqProto::CHECKPOINTING_MODE_DISABLED);
            Y_ABORT_UNLESS(Checkpoints);
            const auto& checkpoint = channelData.GetCheckpoint();
            inputChannel->Pause(checkpoint);
            Checkpoints->RegisterCheckpoint(checkpoint, channelData.GetChannelId());
        }

        TakeInputChannelDataRequests[Cookie++] = TTakeInputChannelData{ack, channelData.GetChannelId(), watermark};
    }

    void PassAway() override {
        if (TaskRunnerActor) {
            TaskRunnerActor->PassAway();
        }
        if (UseCpuQuota() && CpuTimeSpent.MilliSeconds()) {
            if (CpuTime) {
                CpuTime->Add(CpuTimeSpent.MilliSeconds());
            }
            // Send the rest of CPU time that we haven't taken into account
            Send(QuoterServiceActorId,
                new NKikimr::TEvQuota::TEvRequest(
                    NKikimr::TEvQuota::EResourceOperator::And,
                    { NKikimr::TEvQuota::TResourceLeaf(Task.GetRateLimiter(), Task.GetRateLimiterResource(), CpuTimeSpent.MilliSeconds(), true) },
                    TDuration::Max()));
        }
        TBase::PassAway();
    }

    TMaybe<TInstant> GetWatermarkRequest() {
        if (!WatermarksTracker.HasPendingWatermark()) {
            return Nothing();
        }

        const auto pendingWatermark = *WatermarksTracker.GetPendingWatermark();
        if (WatermarkTakeInputChannelDataRequests.contains(pendingWatermark)) {
            // Not all precending to watermark input channels data has been injected
            return Nothing();
        }

        MetricsReporter.ReportInjectedToTaskRunnerWatermark(pendingWatermark);

        return pendingWatermark;
    }

    TMaybe<NDqProto::TCheckpoint> GetCheckpointRequest() {
        if (!CheckpointRequestedFromTaskRunner && Checkpoints && Checkpoints->HasPendingCheckpoint() && !Checkpoints->ComputeActorStateSaved()) {
            CheckpointRequestedFromTaskRunner = true;
            return Checkpoints->GetPendingCheckpoint();
        }
        return Nothing();
    }

    void DoExecuteImpl() override {
        TrySendAsyncChannelsData();

        PollAsyncInput();
        if (ProcessSourcesState.Inflight == 0) {
            auto req = GetCheckpointRequest();
            CA_LOG_T("DoExecuteImpl: " << (bool) req);
            AskContinueRun(std::move(req), /* checkpointOnly = */ false);
        }
    }

    bool SayHelloOnBootstrap() override {
        return false;
    }

    i64 GetInputChannelFreeSpace(ui64 channelId) const override {
        const TInputChannelInfo* inputChannel = InputChannelsMap.FindPtr(channelId);
        YQL_ENSURE(inputChannel, "task: " << Task.GetId() << ", unknown input channelId: " << channelId);

        return inputChannel->FreeSpace;
    }

    void OnTaskRunnerCreated(NTaskRunnerActor::TEvTaskRunnerCreateFinished::TPtr& ev) {
        const auto& secureParams = ev->Get()->SecureParams;
        const auto& taskParams = ev->Get()->TaskParams;
        const auto& readRanges = ev->Get()->ReadRanges;
        const auto& typeEnv = ev->Get()->TypeEnv;
        const auto& holderFactory = ev->Get()->HolderFactory;
        if (Stat) {
            Stat->AddCounters2(ev->Get()->Sensors);
        }
        TypeEnv = const_cast<NKikimr::NMiniKQL::TTypeEnvironment*>(&typeEnv);
        FillIoMaps(holderFactory, typeEnv, secureParams, taskParams, readRanges, nullptr);

        {
            // say "Hello" to executer
            auto ev = MakeHolder<TEvDqCompute::TEvState>();
            ev->Record.SetState(NDqProto::COMPUTE_STATE_EXECUTING);
            ev->Record.SetTaskId(Task.GetId());

            Send(ExecuterId, ev.Release(), NActors::IEventHandle::FlagTrackDelivery);
        }

        if (DeferredInjectCheckpointEvent) {
            ForwardToCheckpoints(std::move(DeferredInjectCheckpointEvent));
        }
        if (DeferredRestoreFromCheckpointEvent) {
            ForwardToCheckpoints(std::move(DeferredRestoreFromCheckpointEvent));
        }

        ContinueExecute(EResumeSource::CATaskRunnerCreated);
    }

    bool ReadyToCheckpoint() const override {
        return ReadyToCheckpointFlag;
    }

    void OnRunFinished(NTaskRunnerActor::TEvTaskRunFinished::TPtr& ev) {
        if (Stat) {
            Stat->AddCounters2(ev->Get()->Sensors);
        }
        ContinueRunInflight = false;
        TrySendAsyncChannelsData(); // send from previous cycle

        MkqlMemoryLimit = ev->Get()->MkqlMemoryLimit;
        ProfileStats = std::move(ev->Get()->ProfileStats);
        auto status = ev->Get()->RunStatus;

        CA_LOG_T("Resume execution, run status: " << status << " checkpoint: " << (bool) ev->Get()->ProgramState
            << " watermark injected: " << ev->Get()->WatermarkInjectedToOutputs);

        for (const auto& [channelId, freeSpace] : ev->Get()->InputChannelFreeSpace) {
            auto it = InputChannelsMap.find(channelId);
            if (it != InputChannelsMap.end()) {
                it->second.FreeSpace = freeSpace;
            }
        }
        for (const auto& [inputIndex, freeSpace] : ev->Get()->SourcesFreeSpace) {
            auto it = SourcesMap.find(inputIndex);
            if (it != SourcesMap.end()) {
                it->second.FreeSpace = freeSpace;
            }
        }

        if (ev->Get()->WatermarkInjectedToOutputs && !WatermarksTracker.HasOutputChannels()) {
            ResumeInputsByWatermark(*WatermarksTracker.GetPendingWatermark());
            WatermarksTracker.PopPendingWatermark();
        }

        ReadyToCheckpointFlag = (bool) ev->Get()->ProgramState;
        if (ev->Get()->CheckpointRequestedFromTaskRunner) {
            CheckpointRequestedFromTaskRunner = false;
        }
        if (ReadyToCheckpointFlag) {
            Y_ABORT_UNLESS(!ProgramState);
            ProgramState = std::move(ev->Get()->ProgramState);
            Checkpoints->DoCheckpoint();
        }
        ProcessOutputsImpl(status);
        if (status == ERunStatus::Finished) {
            ReportStats(TInstant::Now(), ESendStats::IfPossible);
        }

        if (UseCpuQuota()) {
            CpuTimeSpent += ev->Get()->ComputeTime;
            AskCpuQuota();
            ProcessContinueRun();
        }
    }

    void SaveState(const NDqProto::TCheckpoint& checkpoint, NDqProto::TComputeActorState& state) const override {
        CA_LOG_D("Save state");
        Y_ABORT_UNLESS(ProgramState);
        state.MutableMiniKqlProgram()->Swap(&*ProgramState);
        ProgramState.Destroy();

        // TODO:(whcrc) maybe save Sources before Program?
        for (auto& [inputIndex, source] : SourcesMap) {
            YQL_ENSURE(source.AsyncInput, "Source[" << inputIndex << "] is not created");
            NDqProto::TSourceState& sourceState = *state.AddSources();
            source.AsyncInput->SaveState(checkpoint, sourceState);
            sourceState.SetInputIndex(inputIndex);
        }
    }

    void OnSourceDataAck(NTaskRunnerActor::TEvSourceDataAck::TPtr& ev) {
        auto it = SourcesMap.find(ev->Get()->Index);
        Y_ABORT_UNLESS(it != SourcesMap.end());
        auto& source = it->second;
        source.PushStarted = false;
        source.FreeSpace = ev->Get()->FreeSpaceLeft;
        if (--ProcessSourcesState.Inflight == 0) {
            CA_LOG_T("Send TEvContinueRun on OnAsyncInputPushFinished");
            AskContinueRun(Nothing(), false);
        }
    }

    void OnOutputChannelData(NTaskRunnerActor::TEvOutputChannelData::TPtr& ev) {
        if (Stat) {
            Stat->AddCounters2(ev->Get()->Sensors);
        }
        auto it = OutputChannelsMap.find(ev->Get()->ChannelId);
        Y_ABORT_UNLESS(it != OutputChannelsMap.end());
        TOutputChannelInfo& outputChannel = it->second;
        Y_ABORT_UNLESS(!outputChannel.AsyncData); // have finished previous cycle
        outputChannel.AsyncData.ConstructInPlace();
        outputChannel.AsyncData->Watermark = std::move(ev->Get()->Watermark);
        outputChannel.AsyncData->Data = std::move(ev->Get()->Data);
        outputChannel.AsyncData->Checkpoint = std::move(ev->Get()->Checkpoint);
        outputChannel.AsyncData->Finished = ev->Get()->Finished;
        outputChannel.AsyncData->Changed = ev->Get()->Changed;

        if (TrySendAsyncChannelData(outputChannel)) {
            CheckRunStatus();
        }
    }

    bool TrySendAsyncChannelData(TOutputChannelInfo& outputChannel) {
        if (!outputChannel.AsyncData) {
            return false;
        }

        // If the channel has finished, then the data received after drain is no longer needed
        const bool shouldSkipData = Channels->ShouldSkipData(outputChannel.ChannelId);
        if (!shouldSkipData && !Channels->CanSendChannelData(outputChannel.ChannelId)) { // When channel will be connected, they will call resume execution.
            CA_LOG_T("TrySendAsyncChannelData return false because Channel can't send channel data");
            return false;
        }
        if (!shouldSkipData && !Channels->HasFreeMemoryInChannel(outputChannel.ChannelId)) {
            CA_LOG_T("TrySendAsyncChannelData return false because No free memory in channel");
            return false;
        }

        auto& asyncData = *outputChannel.AsyncData;
        outputChannel.Finished = asyncData.Finished || shouldSkipData;
        if (outputChannel.Finished) {
            FinishedOutputChannels.insert(outputChannel.ChannelId);
        }

        outputChannel.PopStarted = false;
        ProcessOutputsState.Inflight--;
        ProcessOutputsState.HasDataToSend |= !outputChannel.Finished;
        ProcessOutputsState.LastPopReturnedNoData = asyncData.Data.empty();

        if (asyncData.Watermark.Defined()) {
            const auto watermark = TInstant::MicroSeconds(asyncData.Watermark->GetTimestampUs());
            const bool shouldResumeInputs = WatermarksTracker.NotifyOutputChannelWatermarkSent(
                outputChannel.ChannelId,
                watermark
            );

            if (shouldResumeInputs) {
                ResumeInputsByWatermark(watermark);
            }
        }

        if (!shouldSkipData) {
            if (asyncData.Checkpoint.Defined()) {
                ResumeInputsByCheckpoint();
            }

            for (ui32 i = 0; i < asyncData.Data.size(); i++) {
                TDqSerializedBatch& chunk = asyncData.Data[i];
                TChannelDataOOB channelData;
                channelData.Proto.SetChannelId(outputChannel.ChannelId);
                // set finished only for last chunk
                const bool lastChunk = i == asyncData.Data.size() - 1;
                channelData.Proto.SetFinished(asyncData.Finished && lastChunk);
                channelData.Proto.MutableData()->Swap(&chunk.Proto);
                channelData.Payload = std::move(chunk.Payload);
                if (lastChunk && asyncData.Watermark.Defined()) {
                    channelData.Proto.MutableWatermark()->Swap(&*asyncData.Watermark);
                }
                if (lastChunk && asyncData.Checkpoint.Defined()) {
                    channelData.Proto.MutableCheckpoint()->Swap(&*asyncData.Checkpoint);
                }
                Channels->SendChannelData(std::move(channelData), i + 1 == asyncData.Data.size());
            }
            if (asyncData.Data.empty() && asyncData.Changed) {
                TChannelDataOOB channelData;
                channelData.Proto.SetChannelId(outputChannel.ChannelId);
                channelData.Proto.SetFinished(asyncData.Finished);
                if (asyncData.Watermark.Defined()) {
                    channelData.Proto.MutableWatermark()->Swap(&*asyncData.Watermark);
                }
                if (asyncData.Checkpoint.Defined()) {
                    channelData.Proto.MutableCheckpoint()->Swap(&*asyncData.Checkpoint);
                }
                Channels->SendChannelData(std::move(channelData), true);
            }
        }

        ProcessOutputsState.DataWasSent |= asyncData.Changed;

        ProcessOutputsState.AllOutputsFinished =
            FinishedOutputChannels.size() == OutputChannelsMap.size() &&
            FinishedSinks.size() == SinksMap.size();

        outputChannel.AsyncData = Nothing();

        return true;
    }

    bool TrySendAsyncChannelsData() {
        bool result = false;
        for (auto& [channelId, outputChannel] : OutputChannelsMap) {
            result |= TrySendAsyncChannelData(outputChannel);
        }
        if (result) {
            CheckRunStatus();
        }
        return result;
    }

    void OnInputChannelDataAck(NTaskRunnerActor::TEvInputChannelDataAck::TPtr& ev) {
        auto it = TakeInputChannelDataRequests.find(ev->Cookie);
        YQL_ENSURE(it != TakeInputChannelDataRequests.end());

        CA_LOG_T("Input data push finished. Cookie: " << ev->Cookie
            << " Watermark: " << it->second.Watermark
            << " Ack: " << it->second.Ack
            << " TakeInputChannelDataRequests: " << TakeInputChannelDataRequests.size()
            << " WatermarkTakeInputChannelDataRequests: " << WatermarkTakeInputChannelDataRequests.size());

        if (it->second.Watermark.Defined()) {
            auto& ct = WatermarkTakeInputChannelDataRequests.at(*it->second.Watermark);
            if (--ct == 0) {
                WatermarkTakeInputChannelDataRequests.erase(*it->second.Watermark);
            }
        }

        TInputChannelInfo* inputChannel = InputChannelsMap.FindPtr(it->second.ChannelId);
        Y_ABORT_UNLESS(inputChannel);
        inputChannel->FreeSpace = ev->Get()->FreeSpace;

        if (it->second.Ack) {
            Channels->SendChannelDataAck(it->second.ChannelId, inputChannel->FreeSpace);
        }

        TakeInputChannelDataRequests.erase(it);

        ResumeExecution(EResumeSource::AsyncPopFinished);
    }

    void SinkSend(ui64 index,
                  NKikimr::NMiniKQL::TUnboxedValueBatch&& batch,
                  TMaybe<NDqProto::TCheckpoint>&& checkpoint,
                  i64 size,
                  i64 checkpointSize,
                  bool finished,
                  bool changed) override
    {
        auto outputIndex = index;
        auto dataWasSent = finished || changed;
        auto dataSize = size;
        auto it = SinksMap.find(outputIndex);
        Y_ABORT_UNLESS(it != SinksMap.end());

        TAsyncOutputInfoBase& sinkInfo = it->second;
        sinkInfo.Finished = finished;
        if (finished) {
            FinishedSinks.insert(outputIndex);
        }
        if (checkpoint) {
            CA_LOG_I("Resume inputs");
            ResumeInputsByCheckpoint();
        }

        sinkInfo.PopStarted = false;
        ProcessOutputsState.Inflight--;
        ProcessOutputsState.HasDataToSend |= !sinkInfo.Finished;

        {
            auto guard = BindAllocator();
            NKikimr::NMiniKQL::TUnboxedValueBatch data = std::move(batch);
            sinkInfo.AsyncOutput->SendData(std::move(data), size, std::move(checkpoint), finished);
        }

        Y_ABORT_UNLESS(batch.empty());
        CA_LOG_T("Sink " << outputIndex << ": sent " << dataSize << " bytes of data and " << checkpointSize << " bytes of checkpoint barrier");

        CA_LOG_T("Drain sink " << outputIndex
            << ". Free space decreased: " << (sinkInfo.FreeSpaceBeforeSend - sinkInfo.AsyncOutput->GetFreeSpace())
            << ", sent data from buffer: " << dataSize);

        ProcessOutputsState.DataWasSent |= dataWasSent;
        ProcessOutputsState.AllOutputsFinished =
            FinishedOutputChannels.size() == OutputChannelsMap.size() &&
            FinishedSinks.size() == SinksMap.size();
        CheckRunStatus();
    }

    // for logging in the base class
    ui64 GetMkqlMemoryLimit() const override {
        return MkqlMemoryLimit;
    }

    const TDqMemoryQuota::TProfileStats* GetMemoryProfileStats() const override {
        return &ProfileStats;
    }

    const NYql::NDq::TTaskRunnerStatsBase* GetTaskRunnerStats() override {
        return TaskRunnerStats.Get();
    }

    const NYql::NDq::TDqMeteringStats* GetMeteringStats() override {
        // TODO: support async CA
        return nullptr;
    }

    template<typename TSecond>
    TVector<ui32> GetIds(const THashMap<ui64, TSecond>& collection) {
        TVector<ui32> ids;
        std::transform(collection.begin(), collection.end(), std::back_inserter(ids), [](const auto& p) {
            return p.first;
        });
        return ids;
    }

    void InjectBarrierToOutputs(const NDqProto::TCheckpoint&) override {
        Y_ABORT_UNLESS(CheckpointingMode != NDqProto::CHECKPOINTING_MODE_DISABLED);
        // already done in task_runner_actor
    }

    void DoLoadRunnerState(TString&& blob) override {
        Send(TaskRunnerActorId, new NTaskRunnerActor::TEvLoadTaskRunnerFromState(std::move(blob)));
    }

    void OnTaskRunnerLoaded(NTaskRunnerActor::TEvLoadTaskRunnerFromStateDone::TPtr& ev) {
        Checkpoints->AfterStateLoading(std::move(ev->Get()->Error));
    }

    template <class TEvPtr>
    void ForwardToCheckpoints(TEvPtr&& ev) {
        auto* x = reinterpret_cast<TAutoPtr<NActors::IEventHandle>*>(&ev);
        Checkpoints->Receive(*x);
        ev = nullptr;
    }

    void OnInjectCheckpoint(TEvDqCompute::TEvInjectCheckpoint::TPtr& ev) {
        if (TypeEnv) {
            ForwardToCheckpoints(std::move(ev));
        } else {
            Y_ABORT_UNLESS(!DeferredInjectCheckpointEvent);
            DeferredInjectCheckpointEvent = std::move(ev);
        }
    }

    void OnRestoreFromCheckpoint(TEvDqCompute::TEvRestoreFromCheckpoint::TPtr& ev) {
        if (TypeEnv) {
            ForwardToCheckpoints(std::move(ev));
        } else {
            Y_ABORT_UNLESS(!DeferredRestoreFromCheckpointEvent);
            DeferredRestoreFromCheckpointEvent = std::move(ev);
        }
    }

    bool UseCpuQuota() const {
        return QuoterServiceActorId && Task.GetRateLimiter() && Task.GetRateLimiterResource();
    }

    void AskCpuQuota() {
        Y_ABORT_UNLESS(!CpuTimeQuotaAsked);
        if (CpuTimeSpent >= MIN_QUOTED_CPU_TIME) {
            CA_LOG_T("Ask CPU quota: " << CpuTimeSpent.MilliSeconds() << "ms");
            if (CpuTime) {
                CpuTime->Add(CpuTimeSpent.MilliSeconds());
            }
            Send(QuoterServiceActorId,
                new NKikimr::TEvQuota::TEvRequest(
                    NKikimr::TEvQuota::EResourceOperator::And,
                    { NKikimr::TEvQuota::TResourceLeaf(Task.GetRateLimiter(), Task.GetRateLimiterResource(), CpuTimeSpent.MilliSeconds(), true) },
                    TDuration::Max()));
            CpuTimeQuotaAsked = TInstant::Now();
            CpuTimeSpent = TDuration::Zero();
        }
    }

    void AskContinueRun(TMaybe<NDqProto::TCheckpoint>&& checkpointRequest, bool checkpointOnly) {
        Y_ABORT_UNLESS(!checkpointOnly || !ContinueRunEvent);
        if (!ContinueRunEvent) {
            ContinueRunStartWaitTime = TInstant::Now();
            ContinueRunEvent = std::make_unique<NTaskRunnerActor::TEvContinueRun>();
            ContinueRunEvent->SinkIds = GetIds(SinksMap);
            ContinueRunEvent->InputTransformIds = GetIds(InputTransformsMap);
        }
        ContinueRunEvent->CheckpointOnly = checkpointOnly;
        if (TMaybe<TInstant> watermarkRequest = GetWatermarkRequest()) {
            if (!ContinueRunEvent->WatermarkRequest) {
                ContinueRunEvent->WatermarkRequest.ConstructInPlace();
                ContinueRunEvent->WatermarkRequest->Watermark = *watermarkRequest;

                ContinueRunEvent->WatermarkRequest->ChannelIds.reserve(OutputChannelsMap.size());
                for (const auto& [channelId, info] : OutputChannelsMap) {
                    if (info.WatermarksMode != NDqProto::EWatermarksMode::WATERMARKS_MODE_DISABLED) {
                        ContinueRunEvent->WatermarkRequest->ChannelIds.emplace_back(channelId);
                    }
                }
            } else {
                ContinueRunEvent->WatermarkRequest->Watermark = Max(ContinueRunEvent->WatermarkRequest->Watermark, *watermarkRequest);
            }
        }
        if (checkpointRequest) {
            if (!ContinueRunEvent->CheckpointRequest) {
                ContinueRunEvent->CheckpointRequest.ConstructInPlace(GetIds(OutputChannelsMap), GetIds(SinksMap), *checkpointRequest);
            } else {
                Y_ABORT_UNLESS(ContinueRunEvent->CheckpointRequest->Checkpoint.GetGeneration() == checkpointRequest->GetGeneration());
                Y_ABORT_UNLESS(ContinueRunEvent->CheckpointRequest->Checkpoint.GetId() == checkpointRequest->GetId());
            }
        }

        if (!UseCpuQuota()) {
            Send(TaskRunnerActorId, ContinueRunEvent.release());
            return;
        }

        ProcessContinueRun();
    }

    void ProcessContinueRun() {
        if (ContinueRunEvent && !CpuTimeQuotaAsked && !ContinueRunInflight) {
            Send(TaskRunnerActorId, ContinueRunEvent.release());
            if (CpuTimeQuotaWaitDelay) {
                const TDuration quotaWaitDelay = TInstant::Now() - ContinueRunStartWaitTime;
                CpuTimeQuotaWaitDelay->Collect(quotaWaitDelay.MilliSeconds());
            }
            ContinueRunInflight = true;
        }
    }

    void OnCpuQuotaGiven(NKikimr::TEvQuota::TEvClearance::TPtr& ev) {
        const TDuration delay = TInstant::Now() - CpuTimeQuotaAsked;
        CpuTimeQuotaAsked = TInstant::Zero();
        CA_LOG_T("CPU quota delay: " << delay.MilliSeconds() << "ms");
        if (CpuTimeGetQuotaLatency) {
            CpuTimeGetQuotaLatency->Collect(delay.MilliSeconds());
        }

        if (ev->Get()->Result != NKikimr::TEvQuota::TEvClearance::EResult::Success) {
            InternalError(NYql::NDqProto::StatusIds::INTERNAL_ERROR, TIssue("Error getting CPU quota"));
            return;
        }

        ProcessContinueRun();
    }

    void CheckRunStatus() override {
        if (!TakeInputChannelDataRequests.empty()) {
            CA_LOG_T("AsyncCheckRunStatus: TakeInputChannelDataRequests: " << TakeInputChannelDataRequests.size());
            return;
        }
        TBase::CheckRunStatus();
    }

private:
    NKikimr::NMiniKQL::TTypeEnvironment* TypeEnv = nullptr;
    NTaskRunnerActor::ITaskRunnerActor* TaskRunnerActor = nullptr;
    NActors::TActorId TaskRunnerActorId;
    NTaskRunnerActor::ITaskRunnerActorFactory::TPtr TaskRunnerActorFactory;
    TEvDqCompute::TEvInjectCheckpoint::TPtr DeferredInjectCheckpointEvent;
    TEvDqCompute::TEvRestoreFromCheckpoint::TPtr DeferredRestoreFromCheckpointEvent;

    THashSet<ui64> FinishedOutputChannels;
    THashSet<ui64> FinishedSinks;
    struct TProcessSourcesState {
        int Inflight = 0;
    };
    TProcessSourcesState ProcessSourcesState;

    struct TTakeInputChannelData {
        bool Ack;
        ui64 ChannelId;
        TMaybe<TInstant> Watermark;
    };
    THashMap<ui64, TTakeInputChannelData> TakeInputChannelDataRequests;
    // Watermark should be injected to task runner only after all precending data is injected
    // This hash map will help to track the right moment
    THashMap<TInstant, ui32> WatermarkTakeInputChannelDataRequests;
    ui64 Cookie = 0;
    NDq::TDqTaskRunnerStatsView TaskRunnerStats;
    bool ReadyToCheckpointFlag;
    TVector<std::pair<NActors::TActorId, ui64>> WaitingForStateResponse;
    bool SentStatsRequest;
    mutable THolder<NDqProto::TMiniKqlProgramState> ProgramState;
    ui64 MkqlMemoryLimit = 0;
    TDqMemoryQuota::TProfileStats ProfileStats;
    bool CheckpointRequestedFromTaskRunner = false;

    // Cpu quota
    TActorId QuoterServiceActorId;
    TInstant CpuTimeQuotaAsked;
    std::unique_ptr<NTaskRunnerActor::TEvContinueRun> ContinueRunEvent;
    TInstant ContinueRunStartWaitTime;
    bool ContinueRunInflight = false;
    NMonitoring::THistogramPtr CpuTimeGetQuotaLatency;
    NMonitoring::THistogramPtr CpuTimeQuotaWaitDelay;
    NMonitoring::TDynamicCounters::TCounterPtr CpuTime;
};


IActor* CreateDqAsyncComputeActor(const TActorId& executerId, const TTxId& txId, NYql::NDqProto::TDqTask* task,
    IDqAsyncIoFactory::TPtr asyncIoFactory,
    const NKikimr::NMiniKQL::IFunctionRegistry* functionRegistry,
    const TComputeRuntimeSettings& settings, const TComputeMemoryLimits& memoryLimits,
    const NTaskRunnerActor::ITaskRunnerActorFactory::TPtr& taskRunnerActorFactory,
    ::NMonitoring::TDynamicCounterPtr taskCounters,
    const TActorId& quoterServiceActorId,
    bool ownCounters)
{
    return new TDqAsyncComputeActor(executerId, txId, task, std::move(asyncIoFactory),
        functionRegistry, settings, memoryLimits, taskRunnerActorFactory, taskCounters, quoterServiceActorId, ownCounters);
}

} // namespace NDq
} // namespace NYql
