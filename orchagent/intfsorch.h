#ifndef SWSS_INTFSORCH_H
#define SWSS_INTFSORCH_H

#include "orch.h"
#include "portsorch.h"

#include "ipaddresses.h"
#include "ipprefix.h"
#include "macaddress.h"

#include <map>
#include <set>
#include <unordered_map>


extern sai_object_id_t gVirtualRouterId;
extern MacAddress gMacAddress;

struct IntfsEntry
{
    std::set<IpPrefix>  ip_addresses;
    int                 ref_count;

    IntfsEntry(int rc = 0) : ref_count(rc) {};
};

typedef map<string, IntfsEntry> IntfsTable;

struct IntfRouteEntry
{
    IpPrefix prefix;
    string   ifName;
    string   type;

    IntfRouteEntry(IpPrefix p, string n, string t = "subnet")
        : prefix(p), ifName(n), type(t) {};

    inline bool operator==(const IntfRouteEntry &x) const
    {
        return (prefix == x.prefix && ifName == x.ifName);
    }
};

/*
 * Hashmap to keep track of all interface-specific routes in the system.
 * Indexed by the string associated to each interface-route (either ip2me or
 * subnet). Values are formed by a list of elements that keep track of each
 * route IpPrefix, as well as the interface on which it was configured.
 *
 * Example:
 *
 *    Key                                        Value
 * ----------             -------------------------------------------------------
 * 10.1.1.0/24 (subnet)   10.1.1.0/24 eth1, 10.1.1.0/24 eth2, 10.1.1.0/24 eth3
 * 10.1.1.1/32 (ip2me)    10.1.1.1/32 eth1, 10.1.1.10/32 eth2, 10.1.1.255/32 eth3
 * ...
 * fe80:1:1/64 (subnet)   fe80:1:1/64 eth1, fe80:1:1/64 eth2
 * fe80:1::1/128 (ip2me)  fe80:1:1::1/128 eth2, fe80:1:1::1/128 eth1
 * fe80:1::5/128 (ip2me)  fe80:1:1::5/128 eth3
 *
 */
typedef unordered_map<string, list<IntfRouteEntry>> IntfRoutesTable;

class IntfsOrch : public Orch
{
public:
    IntfsOrch(DBConnector *db, string tableName);

    sai_object_id_t getRouterIntfsId(const string&);

    void increaseRouterIntfsRefCount(const string&);
    void decreaseRouterIntfsRefCount(const string&);
private:
    IntfsTable m_syncdIntfses;
    IntfRoutesTable m_intfRoutes;

    void doTask(Consumer &consumer);

    int getRouterIntfsRefCount(const string&);

    bool addRouterIntfs(Port &port);
    bool removeRouterIntfs(Port &port);

    void addSubnetRoute(const Port &port, const IpPrefix &ip_prefix);
    void removeSubnetRoute(const Port &port, const IpPrefix &ip_prefix);

    void addIp2MeRoute(const IpPrefix &ip_prefix);
    void removeIp2MeRoute(const IpPrefix &ip_prefix);

    void createIntfRoutes(const IntfRouteEntry &ifRoute, const Port &port);
    void deleteIntfRoutes(const IntfRouteEntry &intRoute, const Port &port);
    void deleteIntfRoute(const IntfRouteEntry &ifRoute, const Port &port);
    void resurrectIntfRoute(const IntfRouteEntry &ifRoute);
    bool trackIntfRouteOverlap(const IntfRouteEntry &ifRoute);

    IpPrefix getIp2mePrefix(const IpPrefix &ip_prefix);
    
    void addDirectedBroadcast(const Port &port, const IpAddress &ip_addr);
    void removeDirectedBroadcast(const Port &port, const IpAddress &ip_addr);
};

#endif /* SWSS_INTFSORCH_H */
