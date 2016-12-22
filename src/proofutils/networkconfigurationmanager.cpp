﻿#include "networkconfigurationmanager.h"

#include "proofgui/guiapplication.h"
#include "proofcore/proofobject_p.h"
#include "proofcore/settings.h"
#include "proofcore/settingsgroup.h"

#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkInterface>
#include <QProcess>
#include <QFile>
#include <QDir>

static const QString NETWORK_SETTINGS_FILE = "/etc/network/interfaces";
static const QString NETWORK_SETTINGS_FILE_TMP = "/tmp/interfaces_tmp";
static const QString VPN_SETTINGS_PATH = "/etc/openvpn/";

static const int VPN_CHECK_WAITING_TIMEOUT = 1000 * 60 * 5; // 5  minutes
static const int VPN_CHECK_INTERVAL = 1000; // 1 second

static const QString STATIC_IP = "static";
static const QString DYNAMIC_IP = "dhcp";

static const QString ADDRESS = "address";
static const QString NETMASK = "netmask";
static const QString GATEWAY = "gateway";
static const QString DNS_NAMESERVERS = "dns-nameservers";

static const QString REGEXP_IP("(((?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?))");

#ifdef Q_OS_LINUX
static const int REQUEST_NETWORK_CONFIGURATION_RETRIES_COUNT = 1;
#else
static const int REQUEST_NETWORK_CONFIGURATION_RETRIES_COUNT = 20;
#endif

static const QHash<QString, QNetworkProxy::ProxyType> PROXY_TYPES = {
    {"http", QNetworkProxy::ProxyType::HttpProxy},
    {"caching http", QNetworkProxy::ProxyType::HttpCachingProxy},
    {"socks5", QNetworkProxy::ProxyType::Socks5Proxy},
    {"caching ftp", QNetworkProxy::ProxyType::FtpCachingProxy}
};

namespace {

struct ProxySettings {
    bool enabled = false;
    QString host;
    quint16 port = 0;
    QString type;
    QString userName;
    QString password;
};

class WorkerThread : public QThread
{
    Q_OBJECT
public:
    explicit WorkerThread(Proof::NetworkConfigurationManagerPrivate *networkConfigurationManager);
    void fetchNetworkInterfaces();
    void checkPassword(const QString &password);
    void fetchNetworkConfiguration(const QString &networkAdapterDescription);
    void writeNetworkConfiguration(const QString &networkAdapterDescription, bool dhcpEnabled, const QString &ipv4Address, const QString &subnetMask,
                                   const QString &gateway, const QString &preferredDns, const QString &alternateDns, const QString &password);
    void fetchVpnConfiguration();
    void writeVpnConfiguration(const QString &absoluteFilePath, const QString &configuration, const QString &password);
    void turnOnVpn(const QString &password);
    void turnOffVpn(const QString &password);

private:
    Proof::NetworkConfigurationManagerPrivate *networkConfigurationManager = nullptr;
};

}

namespace Proof {

class ProxyFactory : public QNetworkProxyFactory
{
public:
    ProxyFactory(const QNetworkProxy &mainProxy, const QStringList &excludes);
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &query) override;

private:
    QNetworkProxy m_mainProxy;
    QNetworkProxy m_emptyProxy;
    QStringList m_excludes;
};

struct NetworkConfiguration {
    QString description;
    QString index;
    bool dhcpEnabled = false;
    QString ipv4Address;
    QString subnetMask;
    QString gateway;
    QString preferredDns;
    QString alternateDns;
};

class NetworkConfigurationManagerPrivate : public ProofObjectPrivate
{
    Q_DECLARE_PUBLIC(NetworkConfigurationManager)
public:
    explicit NetworkConfigurationManagerPrivate(Settings *settings);

    void checkPassword(const QString &password);
    void fetchNetworkInterfaces();
    void fetchNetworkConfiguration(const QString &networkAdapterDescription);
    void writeNetworkConfiguration(const QString &networkAdapterDescription, bool dhcpEnabled, const QString &ipv4Address, const QString &subnetMask,
                                   const QString &gateway, const QString &preferredDns, const QString &alternateDns, const QString &password);
    void fetchProxySettings();
    void writeProxySettings(const ProxySettings &proxySettings);
    void initProxySettings();

    void fetchVpnConfiguration();
    void writeVpnConfiguration(const QString &absoluteFilePath, const QString &configuration, const QString &password);
    void turnOnVpn(const QString &password);
    void turnOffVpn(const QString &password);

private:
    NetworkConfiguration fetchNetworkConfigurationPrivate(const QString &networkAdapterDescription);
    ProxySettings readProxySettingsFromConfig();
    void setProxySettings(const ProxySettings &proxySettings);
    bool enterPassword(QProcess &process, const QString &password);

    WorkerThread *thread = nullptr;
    Settings *appSettings = nullptr;
    ProxySettings lastProxySettings;
    QStringList proxyExcludes;
};

NetworkConfigurationManager::NetworkConfigurationManager(Settings *settings, QObject *parent)
    : ProofObject(*new NetworkConfigurationManagerPrivate(settings), parent)
{
    Q_D(NetworkConfigurationManager);
    d->thread = new WorkerThread(d);
    d->thread->start();

    d->initProxySettings();
}

NetworkConfigurationManager::~NetworkConfigurationManager()
{
    Q_D(NetworkConfigurationManager);
    d->thread->quit();
    d->thread->wait(1000);
    delete d->thread;
}

bool NetworkConfigurationManager::ipSettingsSupported() const
{
#ifdef Q_OS_WIN
    return true;
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    return true;
#endif
    return false;
}

bool NetworkConfigurationManager::vpnSettingsSupported() const
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    return true;
#endif
    return false;
}

bool NetworkConfigurationManager::vpnCanBeControlled()
{
    bool result = false;
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    if (ProofObject::call(this, &NetworkConfigurationManager::vpnCanBeControlled, Proof::Call::Block, result))
        return result;

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    qCDebug(proofUtilsNetworkConfigurationLog) << "Checking OpenVPN service existance";
    process.start("/usr/sbin/service --status-all");
    process.waitForStarted();
    if (process.error() != QProcess::UnknownError) {
        qCDebug(proofUtilsNetworkConfigurationLog) << "OpenVPN service can't be checked" << process.errorString();
        return false;
    }

    if (!process.waitForFinished(5000)) {
        qCDebug(proofUtilsNetworkConfigurationLog) << "OpenVPN service can't be checked and checker will be killed";
        process.kill();
        return false;
    }

    result = process.readAll().contains("openvpn");
    qCDebug(proofUtilsNetworkConfigurationLog) << "OpenVPN service found:" << result;
#endif
    return result;
}

bool NetworkConfigurationManager::passwordSupported() const
{
#ifdef Q_OS_LINUX
    return true;
#else
    return false;
#endif
}

void NetworkConfigurationManager::checkPassword(const QString &password)
{
    Q_D(NetworkConfigurationManager);
    d->checkPassword(password);
}

QVariantMap NetworkConfigurationManager::addresses() const
{
    QVariantMap addresses;
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        for (const auto &address : interface.addressEntries()) {
            auto ip = address.ip();
            if (ip.isLoopback())
                continue;
            QVariantList list = addresses.value(interface.humanReadableName(), QVariantList{}).toList();
            list << ip.toString();
            addresses[interface.humanReadableName()] = list;
        }
    }
    return addresses;
}

void NetworkConfigurationManager::fetchNetworkInterfaces()
{
    Q_D(NetworkConfigurationManager);
    return d->fetchNetworkInterfaces();
}

void NetworkConfigurationManager::fetchNetworkConfiguration(const QString &networkAdapterDescription)
{
    Q_D(NetworkConfigurationManager);
    d->fetchNetworkConfiguration(networkAdapterDescription);
}

void NetworkConfigurationManager::writeNetworkConfiguration(const QString &networkAdapterDescription, bool dhcpEnabled, const QString &ipv4Address, const QString &subnetMask,
                                                            const QString &gateway, const QString &preferredDns, const QString &alternateDns, const QString &password)
{
    Q_D(NetworkConfigurationManager);
    d->writeNetworkConfiguration(networkAdapterDescription, dhcpEnabled, ipv4Address,
                                 subnetMask, gateway, preferredDns, alternateDns, password);
}

QStringList NetworkConfigurationManager::proxyTypes() const
{
    return PROXY_TYPES.keys();
}

void NetworkConfigurationManager::fetchProxySettings()
{
    Q_D(NetworkConfigurationManager);
    d->fetchProxySettings();
}

void NetworkConfigurationManager::writeProxySettings(bool enabled, const QString &host, quint16 port, const QString &type, const QString &userName, const QString &password)
{
    Q_D(NetworkConfigurationManager);
    ProxySettings proxySettings;
    proxySettings.enabled = enabled;
    proxySettings.host = host;
    proxySettings.port = port;
    proxySettings.type = type;
    proxySettings.userName = userName;
    proxySettings.password = password;
    d->writeProxySettings(proxySettings);
}

bool NetworkConfigurationManager::vpnEnabled()
{
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        if (interface.flags().testFlag(QNetworkInterface::IsPointToPoint))
            return true;
    }
    return false;
}

NetworkConfigurationManagerPrivate::NetworkConfigurationManagerPrivate(Settings *settings)
    : ProofObjectPrivate()
{
    appSettings = settings;
}

void NetworkConfigurationManager::fetchVpnConfiguration()
{
    Q_D(NetworkConfigurationManager);
    d->fetchVpnConfiguration();
}

void NetworkConfigurationManager::writeVpnConfiguration(const QString &absoluteFilePath, const QString &configuration, const QString &password)
{
    Q_D(NetworkConfigurationManager);
    d->writeVpnConfiguration(absoluteFilePath, configuration, password);
}

void NetworkConfigurationManager::turnOnVpn(const QString &password)
{
    Q_D(NetworkConfigurationManager);
    d->turnOnVpn(password);
}

void NetworkConfigurationManager::turnOffVpn(const QString &password)
{
    Q_D(NetworkConfigurationManager);
    d->turnOffVpn(password);
}

void NetworkConfigurationManager::skipSwitchingVpnCheck()
{
    if (m_checkVpnStateTimerId != -1) {
        killTimer(m_checkVpnStateTimerId);
        m_checkVpnStateTimerId = -1;
        m_currentTimeCheckVpn = 0;
        m_vpnState = VpnState::Other;
    }
}

void NetworkConfigurationManager::timerEvent(QTimerEvent *event)
{
    int timerId = event->timerId();
    if (timerId == m_checkVpnStateTimerId) {
        if (vpnEnabled() && m_vpnState == VpnState::Starting) {
            skipSwitchingVpnCheck();
            emit vpnTurnedOn();
        } else if (!vpnEnabled() && m_vpnState == VpnState::Stopping) {
            skipSwitchingVpnCheck();
            emit vpnTurnedOff();
        } else {
            m_currentTimeCheckVpn += VPN_CHECK_INTERVAL;
            if (m_currentTimeCheckVpn >= VPN_CHECK_WAITING_TIMEOUT) {
                switch (m_vpnState) {
                case VpnState::Starting:
                    emit errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::VpnCannotBeStarted, QObject::tr("VPN can't be started"), true);
                    break;
                case VpnState::Stopping:
                    emit errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::VpnCannotBeStopped, QObject::tr("VPN can't be stopped"), true);
                    break;
                default:
                    break;
                }
                skipSwitchingVpnCheck();
            }
        }
    }
}

void NetworkConfigurationManager::startTimerForCheckVpnState(VpnState vpnState)
{
    if (ProofObject::call(this, &NetworkConfigurationManager::startTimerForCheckVpnState, vpnState))
        return;
    skipSwitchingVpnCheck();
    m_vpnState = vpnState;
    m_checkVpnStateTimerId = startTimer(VPN_CHECK_INTERVAL);
}

void NetworkConfigurationManagerPrivate::checkPassword(const QString &password)
{
    Q_Q(NetworkConfigurationManager);
    if (ProofObject::call(thread, &WorkerThread::checkPassword, password))
        return;

#ifdef Q_OS_LINUX
    QProcess checker;
    checker.setProcessChannelMode(QProcess::MergedChannels);
    checker.start(QString("sudo -S -k pwd"));
    if (checker.error() == QProcess::UnknownError)
        emit q->passwordChecked(enterPassword(checker, password));
    else
        qCDebug(proofUtilsNetworkConfigurationLog) << "Process couldn't be started" << checker.error() << checker.errorString();
#else
    Q_UNUSED(password);
    qCDebug(proofUtilsNetworkConfigurationLog) << "Password check is not supported for this platform";
    emit q->passwordChecked(true);
#endif
}

void NetworkConfigurationManagerPrivate::fetchNetworkInterfaces()
{
    Q_Q(NetworkConfigurationManager);
    if (ProofObject::call(thread, &WorkerThread::fetchNetworkInterfaces))
        return;

    QStringList result;
#ifdef Q_OS_WIN
    QProcess readInfoProcess;
    readInfoProcess.setReadChannel(QProcess::StandardOutput);
    readInfoProcess.start("PowerShell", {"-command", "Get-WmiObject win32_NetworkAdapter | %{ if ($_.PhysicalAdapter) {$_.MACAddress} }"});
    readInfoProcess.waitForStarted();
    QStringList macAddresses;
    bool powerShellError = readInfoProcess.error() != QProcess::UnknownError;
    if (powerShellError) {
        qCDebug(proofUtilsNetworkConfigurationLog) <<  "PowerShell can't be started:" << readInfoProcess.errorString();
    } else {
        readInfoProcess.waitForFinished();
        macAddresses = QString(readInfoProcess.readAll()).split("\r\n" , QString::SkipEmptyParts);
    }

    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        if (macAddresses.contains(interface.hardwareAddress()) || (powerShellError && !interface.flags().testFlag(QNetworkInterface::IsLoopBack)))
            result.append(interface.humanReadableName());
    }
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    QFile settingsFile(NETWORK_SETTINGS_FILE);
    if (settingsFile.open(QIODevice::ReadOnly)) {
        while (!settingsFile.atEnd()) {
            QString line = QString(settingsFile.readLine()).trimmed();
            if (line.startsWith("iface")) {
                QStringList ifaceLine = line.split(" ", QString::SkipEmptyParts);
                if (ifaceLine.count() > 1)
                    result.append(ifaceLine.at(1));
            }
        }
    } else {
        qCWarning(proofUtilsNetworkConfigurationLog) << NETWORK_SETTINGS_FILE <<  "can't be opened:" << settingsFile.errorString();
    }

    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        if (interface.flags().testFlag(QNetworkInterface::IsLoopBack) || interface.flags().testFlag(QNetworkInterface::IsPointToPoint))
            result.removeAll(interface.humanReadableName());
        else if (!result.contains(interface.humanReadableName()))
            result.append(interface.humanReadableName());
    }
#endif

    emit q->networkInterfacesFetched(result);
}

void NetworkConfigurationManagerPrivate::fetchNetworkConfiguration(const QString &networkAdapterDescription)
{
    if (ProofObject::call(thread, &WorkerThread::fetchNetworkConfiguration, networkAdapterDescription))
        return;

    Q_Q(NetworkConfigurationManager);
    NetworkConfiguration networkConfiguration = fetchNetworkConfigurationPrivate(networkAdapterDescription);
    emit q->networkConfigurationFetched(networkConfiguration.dhcpEnabled, networkConfiguration.ipv4Address, networkConfiguration.subnetMask,
                                        networkConfiguration.gateway, networkConfiguration.preferredDns, networkConfiguration.alternateDns);
}

NetworkConfiguration NetworkConfigurationManagerPrivate::fetchNetworkConfigurationPrivate(const QString &networkAdapterDescription)
{
    Q_Q(NetworkConfigurationManager);

    NetworkConfiguration networkConfiguration;
    networkConfiguration.description = networkAdapterDescription;
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        if (interface.humanReadableName() == networkAdapterDescription) {
            networkConfiguration.index = QString::number(interface.index());
            break;
        }
    }

#ifdef Q_OS_WIN
    QProcess readInfoProcess;
    readInfoProcess.setReadChannel(QProcess::StandardOutput);
    QString splitSimbol = "\";\"";
    readInfoProcess.start("PowerShell", {"-command", QString("$WMI = Get-WmiObject -Class Win32_NetworkAdapterConfiguration -Filter {InterfaceIndex LIKE '%1'};"
                                         "$WMI.DHCPEnabled; %2; $WMI.IPAddress; %2; $WMI.IPSubnet; %2; $WMI.DefaultIPGateway; %2; $WMI.DNSServerSearchOrder;").arg(networkConfiguration.index).arg(splitSimbol)});
    readInfoProcess.waitForStarted();
    if (readInfoProcess.error() != QProcess::UnknownError) {
        qCDebug(proofUtilsNetworkConfigurationLog) <<  "PowerShell can't be started:" << readInfoProcess.errorString();
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::NetworkConfigurationCannotBeWritten, QObject::tr("Can't read network configuration"), true);
        return NetworkConfiguration();
    }
    readInfoProcess.waitForFinished();
    QStringList adaptersInfo = QString(readInfoProcess.readAll()).split(splitSimbol.remove("\"") , QString::SkipEmptyParts);
    for (int i = 0; i < adaptersInfo.count(); ++i) {
        if (i == 0) {
            networkConfiguration.dhcpEnabled = adaptersInfo.at(i).toLower().contains("true");
            continue;
        }

        QRegExp regExpIp(REGEXP_IP);
        if (regExpIp.indexIn(adaptersInfo.at(i)) != -1) {
            QString ip = regExpIp.cap();
            switch (i) {
            case 1:
                networkConfiguration.ipv4Address = ip;
                break;
            case 2:
                networkConfiguration.subnetMask = ip;
                break;
            case 3:
                networkConfiguration.gateway = ip;
                break;
            case 4:
                networkConfiguration.preferredDns = ip;
                regExpIp.indexIn(QString(adaptersInfo.at(i)).remove(networkConfiguration.preferredDns));
                networkConfiguration.alternateDns = regExpIp.cap();
                break;
            default:
                break;
            }
        }
    }
#else
    QFile settingsFile(NETWORK_SETTINGS_FILE);
    if (!settingsFile.open(QIODevice::ReadOnly)) {
        qCDebug(proofUtilsNetworkConfigurationLog) << NETWORK_SETTINGS_FILE <<  "can't be opened:" << settingsFile.errorString();
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::NetworkConfigurationCannotBeWritten, QObject::tr("Can't read network configuration"), true);
        return NetworkConfiguration();
    }
    bool isParsingInterface = false;
    QRegExp regExpIp(REGEXP_IP);
    while (!settingsFile.atEnd()) {
        QString line = QString(settingsFile.readLine()).trimmed();
        if (line.startsWith("iface")) {
            if (isParsingInterface) {
                break;
            } else if (line.contains(networkAdapterDescription)) {
                isParsingInterface = true;
                networkConfiguration.dhcpEnabled = !line.contains(STATIC_IP);
            }
        } else if (!isParsingInterface) {
            continue;
        } else if (regExpIp.indexIn(line) != -1) {
            if (line.startsWith(ADDRESS)) {
                networkConfiguration.ipv4Address = regExpIp.cap();
            } else if (line.startsWith(NETMASK)) {
                networkConfiguration.subnetMask = regExpIp.cap();
            } else if (line.startsWith(GATEWAY)) {
                networkConfiguration.gateway = regExpIp.cap();
            } else if (line.startsWith(DNS_NAMESERVERS)) {
                int pos = regExpIp.indexIn(line);
                networkConfiguration.preferredDns = regExpIp.cap();
                line.remove(pos, networkConfiguration.preferredDns.length());
                if (regExpIp.indexIn(line) != -1)
                    networkConfiguration.alternateDns = regExpIp.cap();
            }
        }
    }
#endif

    return networkConfiguration;
}

bool NetworkConfigurationManagerPrivate::enterPassword(QProcess &process, const QString &password)
{
    QByteArray readBuffer;
    QByteArray currentRead;

    if (!process.waitForReadyRead()) {
        if (process.state() == QProcess::NotRunning)
            qCDebug(proofUtilsNetworkConfigurationLog) << "No answer from command and process finished. Exitcode =" << process.exitCode();
        else
            qCDebug(proofUtilsNetworkConfigurationLog) << "No answer from command. Returning";
        return process.state() == QProcess::NotRunning && process.exitCode() == 0;
    }
    currentRead = process.readAll();
    readBuffer.append(currentRead);
    currentRead = currentRead.trimmed();
    if (currentRead.contains("[sudo]") || currentRead.contains("password for")) {
        process.write(QString("%1\n").arg(password).toLatin1());
        process.waitForReadyRead();
        currentRead = process.readAll();
        readBuffer.append(currentRead);
        currentRead = currentRead.trimmed();

        if (currentRead.contains("is not in the sudoers")) {
            qCDebug(proofUtilsNetworkConfigurationLog) << "User not in sudoers list; log:\n" << readBuffer;
            return false;
        }
        if (currentRead.contains("Sorry, try again")) {
            qCDebug(proofUtilsNetworkConfigurationLog) << "Sudo rejected the password; log:\n" << readBuffer;
            return false;
        }
    }
    process.waitForFinished();
    readBuffer.append(process.readAll());
    qCDebug(proofUtilsNetworkConfigurationLog) << "Process output:" << readBuffer;
    qCDebug(proofUtilsNetworkConfigurationLog) << "Exitcode =" << process.exitCode();
    return process.exitCode() == 0;
}

void NetworkConfigurationManagerPrivate::writeNetworkConfiguration(const QString &networkAdapterDescription, bool dhcpEnabled, const QString &ipv4Address, const QString &subnetMask,
                                                                   const QString &gateway, const QString &preferredDns, const QString &alternateDns, const QString &password)
{
    Q_Q(NetworkConfigurationManager);
    if (ProofObject::call(thread, &WorkerThread::writeNetworkConfiguration, networkAdapterDescription, dhcpEnabled, ipv4Address, subnetMask,
                          gateway, preferredDns, alternateDns, password))
        return;

    QString adapterIndex;
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        if (interface.humanReadableName() == networkAdapterDescription) {
            adapterIndex = QString::number(interface.index());
            break;
        }
    }
#ifdef Q_OS_WIN
    Q_UNUSED(password);
    QProcess writeInfoProcess;
    writeInfoProcess.setReadChannel(QProcess::StandardOutput);
    QString query;
    query.append(" -Verb RunAs");
    query.append(" -FilePath PowerShell");
    query.append(" -WindowStyle hidden");
    QString argumentList;
    argumentList.append(QString(" $WMI = Get-WmiObject -Class Win32_NetworkAdapterConfiguration -Filter {InterfaceIndex LIKE '%1'};").arg(adapterIndex));
    argumentList.append(dhcpEnabled ? " $WMI.EnableDHCP();" : QString(" $WMI.EnableStatic(\"{%1}\", \"{%2}\");").arg(ipv4Address).arg(subnetMask));
    argumentList.append(QString("$WMI.SetGateways(%1);").arg(gateway.isEmpty() || dhcpEnabled  ? "" : QString("\"{%1}\"").arg(gateway)));
    QString dnsServers;
    if (!dhcpEnabled) {
        for (const auto &dnsServer : {preferredDns, alternateDns}) {
            if (dnsServer.isEmpty())
                continue;
            if (!dnsServers.isEmpty())
                dnsServers.append(",");
            dnsServers.append(QString("\"{%1}\"").arg(dnsServer));
        }
    }
    if (!dnsServers.isEmpty())
        argumentList.append(QString(" $dnsServers = %1;").arg(dnsServers));
    argumentList.append(QString(" $WMI.SetDNSServerSearchOrder(%1);").arg(dnsServers.isEmpty() ? "" : "$dnsServers"));

    query.append(" -ArgumentList { " + argumentList + " }");

    writeInfoProcess.start("PowerShell", {"Start-Process", query});
    writeInfoProcess.waitForStarted();
    if (writeInfoProcess.error() != QProcess::UnknownError) {
        qCDebug(proofUtilsNetworkConfigurationLog) << "PowerShell can't be started:" << writeInfoProcess.errorString();
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::NetworkConfigurationCannotBeWritten, QObject::tr("Can't write network configuration"), true);

        return;
    }
    writeInfoProcess.waitForFinished();

#else
    QProcess networkingProcess;
    networkingProcess.setProcessChannelMode(QProcess::MergedChannels);
    qCDebug(proofUtilsNetworkConfigurationLog) << "Running ifdown";
    networkingProcess.start("sudo -S -k /sbin/ifdown " + networkAdapterDescription);
    networkingProcess.waitForStarted();
    if (networkingProcess.error() != QProcess::UnknownError) {
        qCDebug(proofUtilsNetworkConfigurationLog) << "service networking can't be stopped:" << networkingProcess.errorString();
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::NetworkConfigurationCannotBeWritten, QObject::tr("Can't write network configuration"), true);
        return;
    }

    //We don't need any check for proper ifdown finish because system can decline it due to no presence of iface in config file
    enterPassword(networkingProcess, password);

    QFile settingsFile(NETWORK_SETTINGS_FILE);
    if (!settingsFile.open(QIODevice::ReadOnly)) {
        qCDebug(proofUtilsNetworkConfigurationLog) << NETWORK_SETTINGS_FILE <<  "can't be opened:" << settingsFile.errorString();
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::NetworkConfigurationCannotBeWritten, QObject::tr("Can't write network configuration"), true);
        return;
    }

    QByteArray newInterfaces;
    bool hideIpsForCurrentInterface = false;
    bool interfaceUpdated = false;
    bool autoDirectiveFound = false;
    bool allowHotplugDirectiveFound = false;
    while (!settingsFile.atEnd()) {
        QString line = settingsFile.readLine();
        QString trimmedLine = line.trimmed();
        bool currentInterfaceFound = trimmedLine.startsWith("iface " + networkAdapterDescription);
        if (!autoDirectiveFound)
            autoDirectiveFound = trimmedLine.startsWith("auto " + networkAdapterDescription);
        if (!allowHotplugDirectiveFound)
            allowHotplugDirectiveFound = trimmedLine.startsWith("allow-hotplug " + networkAdapterDescription);

        if (settingsFile.atEnd() && !currentInterfaceFound && !interfaceUpdated) {
            newInterfaces.append(line);
            line = QString("iface %1 inet %2\n").arg(networkAdapterDescription).arg(dhcpEnabled ? DYNAMIC_IP : STATIC_IP);
            currentInterfaceFound = true;
        }

        if (currentInterfaceFound) {
            hideIpsForCurrentInterface = true;
            if (line.contains(STATIC_IP) == dhcpEnabled)
                line.replace(dhcpEnabled ? STATIC_IP : DYNAMIC_IP, dhcpEnabled ? DYNAMIC_IP : STATIC_IP);
            if (!dhcpEnabled) {
                if (!ipv4Address.isEmpty())
                    line.append(ADDRESS + " " + ipv4Address + "\n");
                if (!subnetMask.isEmpty())
                    line.append(NETMASK + " " + subnetMask + "\n");
                if (!gateway.isEmpty())
                    line.append(GATEWAY + " " + gateway + "\n");
                if (!preferredDns.isEmpty() || !alternateDns.isEmpty())
                    line.append(DNS_NAMESERVERS + " " + QStringList({preferredDns, alternateDns}).join(" ") + "\n");
            }
            newInterfaces.append(line);
            interfaceUpdated = true;
        } else if (trimmedLine.startsWith("iface") || trimmedLine.startsWith("auto")
                   || trimmedLine.startsWith("mapping") || trimmedLine.startsWith("source")
                   || trimmedLine.startsWith("no-auto-down") || trimmedLine.startsWith("no-scripts")
                   || trimmedLine.startsWith("allow-") || trimmedLine.isEmpty()) {
            hideIpsForCurrentInterface = false;
        }

        if (!hideIpsForCurrentInterface)
            newInterfaces.append(line);
    }
    settingsFile.close();
    if (!autoDirectiveFound)
        newInterfaces.append("auto " + networkAdapterDescription + "\n");
    if (!allowHotplugDirectiveFound)
        newInterfaces.append("allow-hotplug " + networkAdapterDescription + "\n");

    QFile settingsFileTmp(NETWORK_SETTINGS_FILE_TMP);
    if (!settingsFileTmp.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        qCWarning(proofUtilsNetworkConfigurationLog) << NETWORK_SETTINGS_FILE <<  "can't be opened:" << settingsFileTmp.errorString();
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::NetworkConfigurationCannotBeWritten, QObject::tr("Can't write network configuration"), true);
        return;
    }
    settingsFileTmp.write(newInterfaces);
    settingsFileTmp.close();

    qCDebug(proofUtilsNetworkConfigurationLog) << "Copying new interfaces config to" << NETWORK_SETTINGS_FILE;
    networkingProcess.start("sudo -S -k /bin/cp \"" + NETWORK_SETTINGS_FILE_TMP + "\" \"" + NETWORK_SETTINGS_FILE + "\"");
    networkingProcess.waitForStarted();
    if (networkingProcess.error() != QProcess::UnknownError) {
        qCDebug(proofUtilsNetworkConfigurationLog) << NETWORK_SETTINGS_FILE + " can't be rewritten:" << networkingProcess.errorString();
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::NetworkConfigurationCannotBeWritten, QObject::tr("Can't write network configuration"), true);
        return;
    }

    if (!enterPassword(networkingProcess, password)) {
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::NetworkConfigurationCannotBeWritten, QObject::tr("Can't write network configuration"), true);
        settingsFileTmp.remove();
        return;
    }
    settingsFileTmp.remove();

    qCDebug(proofUtilsNetworkConfigurationLog) << "Running ifup";
    networkingProcess.start("sudo -S -k /sbin/ifup " + networkAdapterDescription);
    networkingProcess.waitForStarted();
    if (networkingProcess.error() != QProcess::UnknownError) {
        qCDebug(proofUtilsNetworkConfigurationLog) << "service networking can't be started:" << networkingProcess.errorString();
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::NetworkConfigurationCannotBeWritten, QObject::tr("Can't write network configuration"), true);
        return;
    }

    if (!enterPassword(networkingProcess, password)) {
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::NetworkConfigurationCannotBeWritten, QObject::tr("Can't write network configuration"), true);
        return;
    }
#endif

    bool successMatch = false;
    bool indexMatch = false;
    bool dhcpMatch = false;
    bool ipSettingsMatch = false;
    bool preferredDnsMatch = false;
    bool alternateDnsMatch= false;
    bool ipAndDnsSettingsMatch = false;
    NetworkConfiguration networkConfiguration;
    for (int i = 0; i < REQUEST_NETWORK_CONFIGURATION_RETRIES_COUNT; ++i) {
        networkConfiguration = fetchNetworkConfigurationPrivate(networkAdapterDescription);
        indexMatch = networkConfiguration.index == adapterIndex;
        dhcpMatch = networkConfiguration.dhcpEnabled == dhcpEnabled;
        ipSettingsMatch = networkConfiguration.ipv4Address == ipv4Address && networkConfiguration.subnetMask == subnetMask && networkConfiguration.gateway == gateway;
        preferredDnsMatch = networkConfiguration.preferredDns == preferredDns || networkConfiguration.preferredDns == alternateDns;
        alternateDnsMatch = networkConfiguration.alternateDns == alternateDns || networkConfiguration.alternateDns == preferredDns;
        ipAndDnsSettingsMatch = ipSettingsMatch && preferredDnsMatch && alternateDnsMatch;

        successMatch = indexMatch && dhcpMatch && (dhcpEnabled || ipAndDnsSettingsMatch);
        if (successMatch)
            break;

        QThread::msleep(250);
    }

    if (successMatch) {
        emit q->networkConfigurationWritten();
    } else {
        if (!indexMatch)
            qCDebug(proofUtilsNetworkConfigurationLog) << "index -" << "Actual:" << networkConfiguration.index << "Expected:" << adapterIndex;
        if (!dhcpMatch)
            qCDebug(proofUtilsNetworkConfigurationLog) << "dhcp -" << "Actual:" << networkConfiguration.dhcpEnabled << "Expected:" << dhcpEnabled;
        if (!dhcpEnabled) {
            if(!ipSettingsMatch) {
                if (networkConfiguration.ipv4Address != ipv4Address)
                    qCDebug(proofUtilsNetworkConfigurationLog) << "ipv4Address -" << "Actual:" << networkConfiguration.ipv4Address << "Expected:" << ipv4Address;
                if (networkConfiguration.subnetMask != subnetMask)
                    qCDebug(proofUtilsNetworkConfigurationLog) << "subnetMask -" << "Actual:" << networkConfiguration.subnetMask << "Expected:" << subnetMask;
                if (networkConfiguration.gateway != gateway)
                    qCDebug(proofUtilsNetworkConfigurationLog) << "gateway -" << "Actual:" << networkConfiguration.gateway << "Expected:" << gateway;
            }
            if (!preferredDnsMatch)
                qCDebug(proofUtilsNetworkConfigurationLog) << "preferredDns -" << "Actual:" << networkConfiguration.preferredDns << "Expected:" << preferredDns;
            if (!alternateDnsMatch)
                qCDebug(proofUtilsNetworkConfigurationLog) << "alternateDns -" << "Actual:" << networkConfiguration.alternateDns << "Expected:" << alternateDns;
        }

        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::NetworkConfigurationCannotBeWritten, QObject::tr("Can't write network configuration"), true);
    }
}

void NetworkConfigurationManagerPrivate::fetchProxySettings()
{
    Q_Q(NetworkConfigurationManager);
    emit q->proxySettingsFetched(lastProxySettings.enabled, lastProxySettings.host, lastProxySettings.port, lastProxySettings.type, lastProxySettings.userName, lastProxySettings.password);
}

ProxySettings NetworkConfigurationManagerPrivate::readProxySettingsFromConfig()
{
    ProxySettings proxySettings;
    if (!appSettings) {
        qCDebug(proofUtilsNetworkConfigurationLog) << "Application settings not found";
        return proxySettings;
    }

    SettingsGroup *networkProxyGroup = appSettings->group("network_proxy", Settings::NotFoundPolicy::Add);
    proxySettings.host = networkProxyGroup->value("host", "", Settings::NotFoundPolicy::Add).toString();
    proxySettings.enabled = networkProxyGroup->value("enabled", !proxySettings.host.isEmpty(), Settings::NotFoundPolicy::Add).toBool();
    proxySettings.port = networkProxyGroup->value("port", 8080, Settings::NotFoundPolicy::Add).toUInt();
    QString defaultType = PROXY_TYPES.key(QNetworkProxy::ProxyType::HttpProxy);
    proxySettings.type = networkProxyGroup->value("type", defaultType, Settings::NotFoundPolicy::Add).toString().trimmed();
    if (proxySettings.type.isEmpty())
        proxySettings.type = defaultType;
    proxySettings.userName = networkProxyGroup->value("username", "", Settings::NotFoundPolicy::Add).toString();
    proxySettings.password = networkProxyGroup->value("password", "", Settings::NotFoundPolicy::Add).toString();

    if (proxySettings.host.isEmpty())
        proxySettings.enabled = false;
    QStringList notTrimmedExcludes = networkProxyGroup->value("excludes", "", Settings::NotFoundPolicy::Add).toString().split("|");

    proxyExcludes.clear();
    for (auto exclude : notTrimmedExcludes) {
        exclude = exclude.trimmed();
        if (!exclude.isEmpty())
            proxyExcludes << exclude;
    }
    return proxySettings;
}

void NetworkConfigurationManagerPrivate::setProxySettings(const ProxySettings &proxySettings)
{
    if (proxySettings.enabled && !proxySettings.host.isEmpty()) {
        QNetworkProxy proxy;
        proxy.setHostName(proxySettings.host);
        proxy.setPort(proxySettings.port);
        if (!proxySettings.userName.isEmpty())
            proxy.setUser(proxySettings.userName);
        if (!proxySettings.password.isEmpty())
            proxy.setPassword(proxySettings.password);
        proxy.setType(PROXY_TYPES.value(proxySettings.type, QNetworkProxy::ProxyType::HttpProxy));
        QNetworkProxyFactory::setApplicationProxyFactory(new ProxyFactory(proxy, proxyExcludes));
    } else {
        QNetworkProxyFactory::setUseSystemConfiguration(true);
    }

    lastProxySettings = proxySettings;
}

void NetworkConfigurationManagerPrivate::writeProxySettings(const ProxySettings &proxySettings)
{
    Q_Q(NetworkConfigurationManager);

    if (!appSettings) {
        qCDebug(proofUtilsNetworkConfigurationLog) << "Application settings not found";
        return;
    }

    if (!QFile::permissions(appSettings->filePath()).testFlag(QFileDevice::WriteUser)) {
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::ProxyConfigurationCannotBeWritten, QObject::tr("Proxy configuration can't be written. Permission denied"), true);
        return;
    }

    SettingsGroup *networkProxyGroup = appSettings->group("network_proxy", Settings::NotFoundPolicy::Add);
    networkProxyGroup->setValue("enabled", proxySettings.enabled);
    networkProxyGroup->setValue("host", proxySettings.host);
    networkProxyGroup->setValue("port", proxySettings.port);
    networkProxyGroup->setValue("type", proxySettings.type);
    networkProxyGroup->setValue("username", proxySettings.userName);
    networkProxyGroup->setValue("password", proxySettings.password);

    setProxySettings(proxySettings);

    emit q->proxySettingsWritten();
}

void NetworkConfigurationManagerPrivate::initProxySettings()
{
    ProxySettings proxySettings = readProxySettingsFromConfig();
    setProxySettings(proxySettings);
    fetchProxySettings();
}

void NetworkConfigurationManagerPrivate::fetchVpnConfiguration()
{
    Q_Q(NetworkConfigurationManager);
    if (ProofObject::call(thread, &WorkerThread::fetchVpnConfiguration))
        return;

    QFileInfoList fileInfoList = QDir(VPN_SETTINGS_PATH).entryInfoList({"*.conf"});
    if (fileInfoList.count()) {
        QFile file(fileInfoList.first().absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) {
            qCDebug(proofUtilsNetworkConfigurationLog) << file.fileName() << "can't be opened";
            emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::VpnConfigurationNotFound, QObject::tr("VPN configuration can't be opened"), true);
            return;
        }
        emit q->vpnConfigurationFetched(file.fileName(), file.readAll());
    } else {
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::VpnConfigurationNotFound, QObject::tr("VPN configuration not found"), true);
    }
}

void NetworkConfigurationManagerPrivate::writeVpnConfiguration(const QString &absoluteFilePath, const QString &configuration, const QString &password)
{
    Q_Q(NetworkConfigurationManager);
    if (ProofObject::call(thread, &WorkerThread::writeVpnConfiguration, absoluteFilePath, configuration, password))
        return;

    QString fileName = QFileInfo(absoluteFilePath).fileName();
    QFile settingsFileTmp("/tmp/" + fileName);
    if (!settingsFileTmp.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        qCWarning(proofUtilsNetworkConfigurationLog) << settingsFileTmp.fileName() <<  "can't be opened:" << settingsFileTmp.errorString();
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::VpnConfigurationCannotBeWritten, QObject::tr("Can't write VPN configuration"), true);
        return;
    }
    settingsFileTmp.write(configuration.toLocal8Bit());
    settingsFileTmp.close();

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    qCDebug(proofUtilsNetworkConfigurationLog) << "rewrite" << absoluteFilePath;
    process.start("sudo -S -k /bin/cp \"" + settingsFileTmp.fileName() + "\" \"" + absoluteFilePath + "\"");
    process.waitForStarted();
    if (process.error() != QProcess::UnknownError) {
        qCDebug(proofUtilsNetworkConfigurationLog) << absoluteFilePath + " can't be rewritten:" << process.errorString();
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::VpnConfigurationCannotBeWritten, QObject::tr("Can't write VPN configuration"), true);
        return;
    }

    if (!enterPassword(process, password)) {
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::VpnConfigurationCannotBeWritten, QObject::tr("Can't write VPN configuration"), true);
        settingsFileTmp.remove();
        return;
    }

    settingsFileTmp.remove();
    emit q->vpnConfigurationWritten();
}

void NetworkConfigurationManagerPrivate::turnOnVpn(const QString &password)
{
    Q_Q(NetworkConfigurationManager);
    if (ProofObject::call(thread, &WorkerThread::turnOnVpn, password))
        return;

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    qCDebug(proofUtilsNetworkConfigurationLog) << "Starting VPN";
    process.start("sudo -S -k service openvpn restart");
    process.waitForStarted();
    if (process.error() != QProcess::UnknownError) {
        qCDebug(proofUtilsNetworkConfigurationLog) << "VPN can't be started:" << process.errorString();
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::VpnCannotBeStarted, QObject::tr("VPN can't be started"), true);
        return;
    }

    if (!enterPassword(process, password)) {
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::VpnCannotBeStarted, QObject::tr("VPN can't be started. Password is wrong"), true);
        return;
    }

    q->startTimerForCheckVpnState(NetworkConfigurationManager::VpnState::Starting);
}

void NetworkConfigurationManagerPrivate::turnOffVpn(const QString &password)
{
    Q_Q(NetworkConfigurationManager);
    if (ProofObject::call(thread, &WorkerThread::turnOffVpn, password))
        return;

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    qCDebug(proofUtilsNetworkConfigurationLog) << "Stopping VPN";
    process.start("sudo -S -k service openvpn stop");
    process.waitForStarted();
    if (process.error() != QProcess::UnknownError) {
        qCDebug(proofUtilsNetworkConfigurationLog) << "VPN can't be stopped:" << process.errorString();
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::VpnCannotBeStopped, QObject::tr("VPN can't be stopped"), true);
        return;
    }

    if (!enterPassword(process, password)) {
        emit q->errorOccurred(UTILS_MODULE_CODE, UtilsErrorCode::VpnCannotBeStopped, QObject::tr("VPN can't be stopped. Password is wrong"), true);
        return;
    }

    q->startTimerForCheckVpnState(NetworkConfigurationManager::VpnState::Stopping);
}

ProxyFactory::ProxyFactory(const QNetworkProxy &mainProxy, const QStringList &excludes)
    : QNetworkProxyFactory(), m_mainProxy(mainProxy), m_excludes(excludes)
{
    m_emptyProxy.setType(QNetworkProxy::ProxyType::NoProxy);
}

QList<QNetworkProxy> ProxyFactory::queryProxy(const QNetworkProxyQuery &query)
{
    if (QHostAddress(query.peerHostName()).isLoopback())
        return {m_emptyProxy};
    for (const auto &exclude : m_excludes) {
        QRegExp rx(exclude, Qt::CaseInsensitive, QRegExp::PatternSyntax::WildcardUnix);
        if (rx.exactMatch(query.peerHostName()))
            return {m_emptyProxy};
    }
    return {m_mainProxy};
}

} // namespace Proof

WorkerThread::WorkerThread(Proof::NetworkConfigurationManagerPrivate *networkConfigurationManager)
    : QThread(), networkConfigurationManager(networkConfigurationManager)
{
    moveToThread(this);
}

void WorkerThread::fetchNetworkInterfaces()
{
    networkConfigurationManager->fetchNetworkInterfaces();
}

void WorkerThread::checkPassword(const QString &password)
{
    networkConfigurationManager->checkPassword(password);
}

void WorkerThread::fetchNetworkConfiguration(const QString &networkAdapterDescription)
{
    networkConfigurationManager->fetchNetworkConfiguration(networkAdapterDescription);
}

void WorkerThread::writeNetworkConfiguration(const QString &networkAdapterDescription, bool dhcpEnabled, const QString &ipv4Address, const QString &subnetMask,
                                             const QString &gateway, const QString &preferredDns, const QString &alternateDns, const QString &password)
{
    networkConfigurationManager->writeNetworkConfiguration(networkAdapterDescription, dhcpEnabled, ipv4Address, subnetMask,
                                                           gateway, preferredDns, alternateDns, password);
}

void WorkerThread::fetchVpnConfiguration()
{
    networkConfigurationManager->fetchVpnConfiguration();
}

void WorkerThread::writeVpnConfiguration(const QString &absoluteFilePath, const QString &configuration, const QString &password)
{
    networkConfigurationManager->writeVpnConfiguration(absoluteFilePath, configuration, password);
}

void WorkerThread::turnOnVpn(const QString &password)
{
    networkConfigurationManager->turnOnVpn(password);
}

void WorkerThread::turnOffVpn(const QString &password)
{
    networkConfigurationManager->turnOffVpn(password);
}

#include "networkconfigurationmanager.moc"
