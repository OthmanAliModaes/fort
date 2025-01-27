#include "confzonemanager.h"

#include <QLoggingCategory>

#include <sqlite/dbutil.h>
#include <sqlite/sqlitedb.h>
#include <sqlite/sqlitestmt.h>

#include <conf/zone.h>
#include <driver/drivermanager.h>
#include <util/conf/confutil.h>
#include <util/ioc/ioccontainer.h>

#include "confmanager.h"

namespace {

const QLoggingCategory LC("confZone");

const char *const sqlInsertZone = "INSERT INTO zone(zone_id, name, enabled, custom_url,"
                                  "    source_code, url, form_data, text_inline)"
                                  "  VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);";

const char *const sqlUpdateZone = "UPDATE zone"
                                  "  SET name = ?2, enabled = ?3, custom_url = ?4,"
                                  "    source_code = ?5, url = ?6,"
                                  "    form_data = ?7, text_inline = ?8"
                                  "  WHERE zone_id = ?1;";

const char *const sqlSelectZoneIds = "SELECT zone_id FROM zone"
                                     "  WHERE zone_id < ?1 ORDER BY zone_id;";

const char *const sqlDeleteZone = "DELETE FROM zone WHERE zone_id = ?1;";

const char *const sqlDeleteAddressGroupZone = "UPDATE address_group"
                                              "  SET include_zones = include_zones & ?1,"
                                              "    exclude_zones = exclude_zones & ?1"
                                              "  WHERE include_zones <> 0 || exclude_zones <> 0;";

const char *const sqlDeleteAppZone = "UPDATE app"
                                     "  SET accept_zones = accept_zones & ?1,"
                                     "    reject_zones = reject_zones & ?1"
                                     "  WHERE accept_zones <> 0 || reject_zones <> 0;";

const char *const sqlDeleteRuleZone = "UPDATE rule"
                                      "  SET accept_zones = accept_zones & ?1,"
                                      "    reject_zones = reject_zones & ?1"
                                      "  WHERE accept_zones <> 0 || reject_zones <> 0;";

const char *const sqlUpdateZoneName = "UPDATE zone SET name = ?2 WHERE zone_id = ?1;";

const char *const sqlUpdateZoneEnabled = "UPDATE zone SET enabled = ?2 WHERE zone_id = ?1;";

const char *const sqlUpdateZoneResult =
        "UPDATE zone"
        "  SET address_count = ?2, text_checksum = ?3, bin_checksum = ?4,"
        "    source_modtime = ?5, last_run = ?6, last_success = ?7"
        "  WHERE zone_id = ?1;";

bool driverWriteZones(ConfUtil &confUtil, QByteArray &buf, int entrySize, bool onlyFlags = false)
{
    if (entrySize == 0) {
        qCWarning(LC) << "Driver config error:" << confUtil.errorMessage();
        return false;
    }

    auto driverManager = IoC<DriverManager>();
    if (!driverManager->writeZones(buf, entrySize, onlyFlags)) {
        qCWarning(LC) << "Update driver error:" << driverManager->errorMessage();
        return false;
    }

    return true;
}

}

ConfZoneManager::ConfZoneManager(QObject *parent) : QObject(parent) { }

ConfManager *ConfZoneManager::confManager() const
{
    return m_confManager;
}

SqliteDb *ConfZoneManager::sqliteDb() const
{
    return confManager()->sqliteDb();
}

void ConfZoneManager::setUp()
{
    m_confManager = IoCPinned()->setUpDependency<ConfManager>();
}

bool ConfZoneManager::addOrUpdateZone(Zone &zone)
{
    bool ok = true;

    beginTransaction();

    const bool isNew = (zone.zoneId == 0);
    if (isNew) {
        zone.zoneId = DbUtil(sqliteDb(), &ok)
                              .sql(sqlSelectZoneIds)
                              .vars({ ConfUtil::zoneMaxCount() })
                              .getFreeId(/*minId=*/1, /*maxId=*/ConfUtil::zoneMaxCount() - 1);
    } else {
        updateDriverZoneFlag(zone.zoneId, zone.enabled);
    }

    const QVariantList vars = {
        zone.zoneId,
        zone.zoneName,
        zone.enabled,
        zone.customUrl,
        zone.sourceCode,
        zone.url,
        zone.formData,
        zone.textInline,
    };

    if (ok) {
        DbUtil(sqliteDb(), &ok).sql(isNew ? sqlInsertZone : sqlUpdateZone).vars(vars).executeOk();
    }

    commitTransaction(ok);

    if (!ok)
        return false;

    if (isNew) {
        emit zoneAdded();
    } else {
        emit zoneUpdated();
    }

    return true;
}

bool ConfZoneManager::deleteZone(int zoneId)
{
    bool ok = false;

    beginTransaction();

    if (DbUtil(sqliteDb(), &ok).sql(sqlDeleteZone).vars({ zoneId }).executeOk()) {
        const quint32 zoneUnMask = ~(quint32(1) << (zoneId - 1));
        const QVariantList vars = { qint64(zoneUnMask) };

        // Delete the Zone from Address Groups
        DbUtil(sqliteDb(), &ok).sql(sqlDeleteAddressGroupZone).vars(vars).executeOk();

        // Delete the Zone from Programs
        DbUtil(sqliteDb(), &ok).sql(sqlDeleteAppZone).vars(vars).executeOk();

        // Delete the Zone from Rules
        DbUtil(sqliteDb(), &ok).sql(sqlDeleteRuleZone).vars(vars).executeOk();
    }

    commitTransaction(ok);

    if (ok) {
        emit zoneRemoved(zoneId);
    }

    return ok;
}

bool ConfZoneManager::updateZoneName(int zoneId, const QString &zoneName)
{
    bool ok = false;

    beginTransaction();

    const QVariantList vars = { zoneId, zoneName };

    DbUtil(sqliteDb(), &ok).sql(sqlUpdateZoneName).vars(vars).executeOk();

    commitTransaction(ok);

    if (ok) {
        emit zoneUpdated();
    }

    return ok;
}

bool ConfZoneManager::updateZoneEnabled(int zoneId, bool enabled)
{
    bool ok = false;

    beginTransaction();

    const QVariantList vars = { zoneId, enabled };

    DbUtil(sqliteDb(), &ok).sql(sqlUpdateZoneEnabled).vars(vars).executeOk();

    commitTransaction(ok);

    if (ok) {
        emit zoneUpdated();

        updateDriverZoneFlag(zoneId, enabled);
    }

    return ok;
}

bool ConfZoneManager::updateZoneResult(const Zone &zone)
{
    bool ok = false;

    beginTransaction();

    const QVariantList vars = {
        zone.zoneId,
        zone.addressCount,
        zone.textChecksum,
        zone.binChecksum,
        zone.sourceModTime,
        zone.lastRun,
        zone.lastSuccess,
    };

    DbUtil(sqliteDb(), &ok).sql(sqlUpdateZoneResult).vars(vars).executeOk();

    commitTransaction(ok);

    if (ok) {
        emit zoneUpdated();
    }

    return ok;
}

void ConfZoneManager::updateDriverZones(quint32 zonesMask, quint32 enabledMask, quint32 dataSize,
        const QList<QByteArray> &zonesData)
{
    ConfUtil confUtil;
    QByteArray buf;

    const int entrySize = confUtil.writeZones(zonesMask, enabledMask, dataSize, zonesData, buf);

    driverWriteZones(confUtil, buf, entrySize);
}

bool ConfZoneManager::updateDriverZoneFlag(int zoneId, bool enabled)
{
    ConfUtil confUtil;
    QByteArray buf;

    const int entrySize = confUtil.writeZoneFlag(zoneId, enabled, buf);

    return driverWriteZones(confUtil, buf, entrySize, /*onlyFlags=*/true);
}

bool ConfZoneManager::beginTransaction()
{
    return sqliteDb()->beginWriteTransaction();
}

void ConfZoneManager::commitTransaction(bool &ok)
{
    ok = sqliteDb()->endTransaction(ok);
}
