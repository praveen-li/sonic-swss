#ifndef SWSS_FDBORCH_H
#define SWSS_FDBORCH_H

#include "orch.h"
#include "observer.h"
#include "portsorch.h"

struct FdbEntry
{
    MacAddress mac;
    sai_vlan_id_t vlan;

    bool operator<(const FdbEntry& other) const
    {
        return tie(mac, vlan) < tie(other.mac, other.vlan);
    }
};

struct FdbUpdate
{
    FdbEntry entry;
    Port port;
    bool add;
};

struct SavedFdbEntry
{
    FdbEntry entry;
    string type;
};

typedef unordered_map<string, vector<SavedFdbEntry>> fdb_entries_by_port_t;

class FdbOrch: public Orch, public Subject, public Observer
{
public:
    FdbOrch(DBConnector *db, string tableName, PortsOrch *port) :
        Orch(db, tableName),
        m_portsOrch(port),
        m_table(Table(db, tableName))
    {
        m_portsOrch->attach(this);
    }

    ~FdbOrch()
    {
        m_portsOrch->detach(this);
    }

    void update(sai_fdb_event_t, const sai_fdb_entry_t *, sai_object_id_t);
    void update(SubjectType type, void *cntx);
    bool getPort(const MacAddress&, uint16_t, Port&);

private:
    PortsOrch *m_portsOrch;
    set<FdbEntry> m_entries;
    fdb_entries_by_port_t saved_fdb_entries;
    Table m_table;

    void doTask(Consumer& consumer);

    void updateVlanMember(const VlanMemberUpdate&);
    bool addFdbEntry(const FdbEntry&, const string&, const string&);
    bool removeFdbEntry(const FdbEntry&);
};

#endif /* SWSS_FDBORCH_H */
