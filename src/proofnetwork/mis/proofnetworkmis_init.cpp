#include "data/job.h"
#include "data/qmlwrappers/jobqmlwrapper.h"
#include "apihelper.h"

#include "proofnetworkmis_global.h"

Q_LOGGING_CATEGORY(proofNetworkMisDataLog, "proof.network.mis.data")

__attribute__((constructor))
static void libraryInit()
{
    qRegisterMetaType<Proof::Mis::JobQmlWrapper *>("Proof::Mis::JobQmlWrapper *");

    qRegisterMetaType<Proof::Mis::JobSP>("Proof::Mis::JobSP");
    qRegisterMetaType<Proof::Mis::JobWP>("Proof::Mis::JobWP");

    qRegisterMetaType<Proof::Mis::ApiHelper::WorkflowStatus>("Proof::Mis::ApiHelper::WorkflowStatus");
    qRegisterMetaType<Proof::Mis::ApiHelper::TransitionEvent>("Proof::Mis::ApiHelper::TransitionEvent");
}