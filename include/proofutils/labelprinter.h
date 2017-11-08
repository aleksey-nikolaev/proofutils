#ifndef PROOF_UTILS_LABELPRINTER_H
#define PROOF_UTILS_LABELPRINTER_H

#include "proofcore/proofobject.h"
#include "proofutils_global.h"

namespace Proof {

class LabelPrinterPrivate;

struct LabelPrinterParams
{
    LabelPrinterParams() {}

    LabelPrinterParams(const QString &printerHost, const QString &printerName, int printerPort = 8090,
                       bool forceServiceUsage = false, bool strictHardwareCheck = true)
        : printerHost(printerHost), printerName(printerName), printerPort(printerPort),
          forceServiceUsage(forceServiceUsage), strictHardwareCheck(strictHardwareCheck) {}

    QString printerHost;
    QString printerName;
    int printerPort = 0;
    bool forceServiceUsage = false;
    bool strictHardwareCheck = true;
};

class PROOF_UTILS_EXPORT LabelPrinter : public ProofObject
{
    Q_OBJECT
    Q_DECLARE_PRIVATE(LabelPrinter)
public:
    explicit LabelPrinter(const QString &printerHost, const QString &printerName, int printerPort = 8090,
                          bool forceServiceUsage = false, bool strictHardwareCheck = true, QObject *parent = nullptr);
    LabelPrinter(const LabelPrinterParams &params, QObject *parent = nullptr);

    bool printLabel(const QByteArray &label, bool ignorePrinterState = false);
    bool printerIsReady();

protected:
    bool event(QEvent *ev);

signals:
    void errorOccurred(long moduleCode, long errorCode, const QString &errorMessage, bool userFriendly);
};

} // namespace Proof

#endif // PROOF_UTILS_LABELPRINTER_H
