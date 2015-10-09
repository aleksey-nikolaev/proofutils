#ifndef MISAPIHELPER_H
#define MISAPIHELPER_H

#include "proofcore/proofobject.h"
#include "proofnetwork/mis/proofnetworkmis_global.h"

namespace Proof {
namespace Mis {

class PROOF_NETWORK_MIS_EXPORT ApiHelper : public ProofObject
{
    Q_OBJECT
    Q_ENUMS(WorkflowStatus)
    Q_ENUMS(TransitionEvent)
    Q_ENUMS(WorkflowAction)
    Q_ENUMS(PaperSide)
public:
    enum class WorkflowStatus {
        NeedsStatus,
        IsReadyForStatus,
        InProgressStatus,
        SuspendedStatus,
        DoneStatus,
        HaltedStatus,
        UnknownStatus
    };

    enum class WorkflowAction {
        BindingAction,
        BinningAction,
        BoxingAction,
        ColorOptimizingAction,
        ComponentBoxingAction,
        ContainerPackingAction,
        CuttingAction,
        DistributeAction,
        FolderMakingAction,
        MagnetizeAction,
        MailingAction,
        MountingAction,
        OutsourceCuttingAction,
        PdfBuildingAction,
        PlateMakingAction,
        PrintingAction,
        QcAction,
        RoundingAction,
        ScreenImagingAction,
        ScreenMountingAction,
        ScreenPreparationAction,
        ScreenWashingAction,
        ShipBoxingAction,
        ShipLabelAction,
        ShippingAction,
        StagingAction,
        TruckLoadingAction,
        UvCoatingAction,
        UvPdfBuildingAction,
        UnknownAction
    };

    enum class TransitionEvent {
        StartEvent,
        StopEvent,
        AbortEvent,
        SuspendEvent,
        ResumeEvent,
        PerformEvent,
        RevertEvent,
        RequestEvent,
        UnknownEvent
    };

    enum class PaperSide {
        NotSetSide,
        FrontSide,
        BackSide
    };

    static QString workflowStatusToString(WorkflowStatus status);
    static WorkflowStatus workflowStatusFromString(QString statusString, bool *ok = nullptr);
    static QString transitionEventToString(TransitionEvent event);
    static TransitionEvent transitionEventFromString(QString eventString, bool *ok = nullptr);
    static QString workflowActionToString(WorkflowAction action);
    static WorkflowAction workflowActionFromString(QString actionString, bool *ok = nullptr);
    static QString paperSideToString(PaperSide side);
    static PaperSide paperSideFromString(QString sideString, bool *ok = nullptr);

private:
    explicit ApiHelper() : ProofObject(0) {}
};

PROOF_NETWORK_MIS_EXPORT uint qHash(ApiHelper::WorkflowStatus arg, uint seed = 0);
PROOF_NETWORK_MIS_EXPORT uint qHash(ApiHelper::TransitionEvent arg, uint seed = 0);

}
}

#endif // MISAPIHELPER_H
