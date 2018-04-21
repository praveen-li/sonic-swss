#include <cassert>
#include <fstream>
#include <sstream>
#include <map>
#include <net/if.h>

#include "intfsorch.h"
#include "ipprefix.h"
#include "logger.h"
#include "swssnet.h"
#include "tokenize.h"
#include "crmorch.h"

extern sai_object_id_t gVirtualRouterId;

extern sai_router_interface_api_t*  sai_router_intfs_api;
extern sai_route_api_t*             sai_route_api;
extern sai_neighbor_api_t*          sai_neighbor_api;

extern PortsOrch *gPortsOrch;
extern sai_object_id_t gSwitchId;
extern CrmOrch *gCrmOrch;

IntfsOrch::IntfsOrch(DBConnector *db, string tableName) :
        Orch(db, tableName)
{
    SWSS_LOG_ENTER();
}

sai_object_id_t IntfsOrch::getRouterIntfsId(const string &alias)
{
    Port port;
    gPortsOrch->getPort(alias, port);
    assert(port.m_rif_id);
    return port.m_rif_id;
}

void IntfsOrch::increaseRouterIntfsRefCount(const string &alias)
{
    SWSS_LOG_ENTER();

    m_syncdIntfses[alias].ref_count++;
    SWSS_LOG_DEBUG("Router interface %s ref count is increased to %d",
                  alias.c_str(), m_syncdIntfses[alias].ref_count);
}

void IntfsOrch::decreaseRouterIntfsRefCount(const string &alias)
{
    SWSS_LOG_ENTER();

    m_syncdIntfses[alias].ref_count--;
    SWSS_LOG_DEBUG("Router interface %s ref count is decreased to %d",
                  alias.c_str(), m_syncdIntfses[alias].ref_count);
}

void IntfsOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->isInitDone())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        vector<string> keys = tokenize(kfvKey(t), ':');
        string alias(keys[0]);
        IpPrefix ip_prefix(kfvKey(t).substr(kfvKey(t).find(':')+1));

        string scope;
        for (auto i : kfvFieldsValues(t))
        {
            if (fvField(i) == "scope")
            {
                scope = (fvValue(i));
                break;
            }
        }

        if (alias == "eth0" || alias == "docker0" || alias == "Bridge")
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        string op = kfvOp(t);
        if (op == SET_COMMAND)
        {
            if (alias == "lo")
            {
                createIntfRoutes(IntfRouteEntry(ip_prefix, alias),
                                 Port(alias, Port::LOOPBACK));

                m_syncdIntfses[alias] = IntfsEntry();
                it = consumer.m_toSync.erase(it);
                continue;
            }

            Port port;
            if (!gPortsOrch->getPort(alias, port))
            {
                /* TODO: Resolve the dependency relationship and add ref_count to port */

                SWSS_LOG_NOTICE("Missing port associated to ip-address %s being "
                                "added on interface %s ",
                                ip_prefix.to_string().c_str(),
                                alias.c_str());
                it++;
                continue;
            }

            auto it_intfs = m_syncdIntfses.find(alias);
            if (it_intfs == m_syncdIntfses.end())
            {
                /*
                 * New routerIntfs will be created only when at least one global-
                 * scoped address is defined within the interface.
                 */
                if (scope == "global" && addRouterIntfs(port))
                {
                    m_syncdIntfses[alias] = IntfsEntry();
                }
                else
                {
                    it++;
                    continue;
                }
            }

            if (m_syncdIntfses[alias].ip_addresses.count(ip_prefix))
            {
                /* Duplicate entry */
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Creating intfRoutes associated to this interface being added */
            createIntfRoutes(IntfRouteEntry(ip_prefix, alias), port);
            if(port.m_type == Port::VLAN && ip_prefix.isV4())
            {
                addDirectedBroadcast(port, ip_prefix.getBroadcastIp());
            }

            m_syncdIntfses[alias].ip_addresses.insert(ip_prefix);
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            if (alias == "lo")
            {
                deleteIntfRoutes(IntfRouteEntry(ip_prefix, alias),
                                 Port(alias, Port::LOOPBACK));
                it = consumer.m_toSync.erase(it);
                continue;
            }

            Port port;
            /* Cannot locate interface */
            if (!gPortsOrch->getPort(alias, port))
            {
                SWSS_LOG_NOTICE("Missing port associated to ip-address %s being "
                                "deleted on interface %s ",
                                ip_prefix.to_string().c_str(),
                                alias.c_str());

                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Deleting intfRoutes associated to this intf being removed */
            deleteIntfRoutes(IntfRouteEntry(ip_prefix, alias), port);

            auto iter = m_syncdIntfses.find(alias);
            if (iter != m_syncdIntfses.end())
            {
                if (iter->second.ip_addresses.count(ip_prefix))
                {
                    if(port.m_type == Port::VLAN && ip_prefix.isV4())
                    {
                        removeDirectedBroadcast(port, ip_prefix.getBroadcastIp());
                    }
                    iter->second.ip_addresses.erase(ip_prefix); 
                }

                 /* Remove router interface if there's no ip-address left. */
                if (iter->second.ip_addresses.size() == 0)
                {
                    if (removeRouterIntfs(port))
                    {
                        m_syncdIntfses.erase(alias);
                        it = consumer.m_toSync.erase(it);
                    }
                    else
                        it++;
                }
                else
                {
                    it = consumer.m_toSync.erase(it);
                }
            }
            else
            {
                /* Cannot locate the interface */
                it = consumer.m_toSync.erase(it);
            }
        }
    }
}

bool IntfsOrch::addRouterIntfs(Port &port)
{
    SWSS_LOG_ENTER();

    /* Return true if the router interface exists */
    if (port.m_rif_id)
    {
        SWSS_LOG_WARN("Router interface already exists on %s",
                      port.m_alias.c_str());
        return true;
    }

    /* Create router interface if the router interface doesn't exist */
    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    attr.value.oid = gVirtualRouterId;
    attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, gMacAddress.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    switch(port.m_type)
    {
        case Port::PHY:
        case Port::LAG:
            attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
            break;
        case Port::VLAN:
            attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_VLAN;
            break;
        default:
            SWSS_LOG_ERROR("Unsupported port type: %d", port.m_type);
            break;
    }
    attrs.push_back(attr);

    switch(port.m_type)
    {
        case Port::PHY:
            attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
            attr.value.oid = port.m_port_id;
            break;
        case Port::LAG:
            attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
            attr.value.oid = port.m_lag_id;
            break;
        case Port::VLAN:
            attr.id = SAI_ROUTER_INTERFACE_ATTR_VLAN_ID;
            attr.value.oid = port.m_vlan_info.vlan_oid;
            break;
        default:
            SWSS_LOG_ERROR("Unsupported port type: %d", port.m_type);
            break;
    }
    attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
    attr.value.u32 = port.m_mtu;
    attrs.push_back(attr);

    sai_status_t status = sai_router_intfs_api->create_router_interface(&port.m_rif_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create router interface for port %s, rv:%d", port.m_alias.c_str(), status);
        throw runtime_error("Failed to create router interface.");
    }

    gPortsOrch->setPort(port.m_alias, port);

    SWSS_LOG_NOTICE("Create router interface for port %s mtu %u", port.m_alias.c_str(), port.m_mtu);

    return true;
}

bool IntfsOrch::removeRouterIntfs(Port &port)
{
    SWSS_LOG_ENTER();

    if (m_syncdIntfses[port.m_alias].ref_count > 0)
    {
        SWSS_LOG_NOTICE("Router interface is still referenced");
        return false;
    }

    sai_status_t status = sai_router_intfs_api->remove_router_interface(port.m_rif_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove router interface for port %s, rv:%d", port.m_alias.c_str(), status);
        throw runtime_error("Failed to remove router interface.");
    }

    port.m_rif_id = 0;
    gPortsOrch->setPort(port.m_alias, port);

    SWSS_LOG_NOTICE("Remove router interface for port %s", port.m_alias.c_str());

    return true;
}

void IntfsOrch::addSubnetRoute(const Port &port, const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = gVirtualRouterId;
    copy(unicast_route_entry.destination, ip_prefix);
    subnet(unicast_route_entry.destination, unicast_route_entry.destination);

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
    attrs.push_back(attr);

    attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    attr.value.oid = port.m_rif_id;
    attrs.push_back(attr);

    sai_status_t status = sai_route_api->create_route_entry(&unicast_route_entry, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create subnet route to %s from %s, rv:%d",
                       ip_prefix.to_string().c_str(), port.m_alias.c_str(), status);
        throw runtime_error("Failed to create subnet route.");
    }

    SWSS_LOG_NOTICE("Create subnet route to %s from %s",
                    ip_prefix.to_string().c_str(), port.m_alias.c_str());
    increaseRouterIntfsRefCount(port.m_alias);

    if (unicast_route_entry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
    }
    else
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
    }
}

void IntfsOrch::removeSubnetRoute(const Port &port, const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = gVirtualRouterId;
    copy(unicast_route_entry.destination, ip_prefix);
    subnet(unicast_route_entry.destination, unicast_route_entry.destination);

    sai_status_t status = sai_route_api->remove_route_entry(&unicast_route_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove subnet route to %s from %s, rv:%d",
                       ip_prefix.to_string().c_str(), port.m_alias.c_str(), status);
        throw runtime_error("Failed to remove subnet route.");
    }

    SWSS_LOG_NOTICE("Remove subnet route to %s from %s",
                    ip_prefix.to_string().c_str(), port.m_alias.c_str());
    decreaseRouterIntfsRefCount(port.m_alias);

    if (unicast_route_entry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
    }
    else
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
    }
}

void IntfsOrch::addIp2MeRoute(const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = gVirtualRouterId;
    copy(unicast_route_entry.destination, ip_prefix.getIp());

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
    attrs.push_back(attr);

    Port cpu_port;
    gPortsOrch->getCpuPort(cpu_port);

    attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    attr.value.oid = cpu_port.m_port_id;
    attrs.push_back(attr);

    sai_status_t status = sai_route_api->create_route_entry(&unicast_route_entry, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create IP2me route ip:%s, rv:%d", ip_prefix.getIp().to_string().c_str(), status);
        throw runtime_error("Failed to create IP2me route.");
    }

    SWSS_LOG_NOTICE("Create IP2me route ip:%s", ip_prefix.getIp().to_string().c_str());

    if (unicast_route_entry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
    }
    else
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
    }
}

void IntfsOrch::removeIp2MeRoute(const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = gVirtualRouterId;
    copy(unicast_route_entry.destination, ip_prefix.getIp());

    sai_status_t status = sai_route_api->remove_route_entry(&unicast_route_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove IP2me route ip:%s, rv:%d", ip_prefix.getIp().to_string().c_str(), status);
        throw runtime_error("Failed to remove IP2me route.");
    }

    SWSS_LOG_NOTICE("Remove packet action trap route ip:%s", ip_prefix.getIp().to_string().c_str());

    if (unicast_route_entry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
    }
    else
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
    }
}

void IntfsOrch::addDirectedBroadcast(const Port &port, const IpAddress &ip_addr)
{
    sai_status_t status;
    sai_neighbor_entry_t neighbor_entry;
    neighbor_entry.rif_id = port.m_rif_id;
    neighbor_entry.switch_id = gSwitchId;
    copy(neighbor_entry.ip_address, ip_addr);

    sai_attribute_t neighbor_attr;
    neighbor_attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
    memcpy(neighbor_attr.value.mac, MacAddress("ff:ff:ff:ff:ff:ff").getMac(), 6);

    status = sai_neighbor_api->create_neighbor_entry(&neighbor_entry, 1, &neighbor_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create broadcast entry %s rv:%d",
                       ip_addr.to_string().c_str(), status);
        return;
    }

    SWSS_LOG_NOTICE("Add broadcast route for ip:%s", ip_addr.to_string().c_str());
}

void IntfsOrch::removeDirectedBroadcast(const Port &port, const IpAddress &ip_addr)
{
    sai_status_t status;
    sai_neighbor_entry_t neighbor_entry;
    neighbor_entry.rif_id = port.m_rif_id;
    neighbor_entry.switch_id = gSwitchId;
    copy(neighbor_entry.ip_address, ip_addr);

    status = sai_neighbor_api->remove_neighbor_entry(&neighbor_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_ITEM_NOT_FOUND)
        {
            SWSS_LOG_ERROR("No broadcast entry found for %s", ip_addr.to_string().c_str());
        }
        else
        {
            SWSS_LOG_ERROR("Failed to remove broadcast entry %s rv:%d",
                           ip_addr.to_string().c_str(), status);
        }
        return;
    }

    SWSS_LOG_NOTICE("Remove broadcast route ip:%s", ip_addr.to_string().c_str());
}

void IntfsOrch::createIntfRoutes(const IntfRouteEntry &ifRoute,
                                 const Port           &port)
{
    SWSS_LOG_ENTER();

    /*
     * Each newly create interface requires the insertion of two routes in the
     * system: a subnet route and an ip2me one.
     */
    IntfRouteEntry ifSubnetRoute(ifRoute.prefix.getSubnet(),
                                 ifRoute.ifName,
                                 "subnet");
    IntfRouteEntry ifIp2meRoute(getIp2mePrefix(ifRoute.prefix),
                                ifRoute.ifName,
                                "ip2me");

    /*
     * There are two scenarios in which we want to skip the addition of an
     * interface-subnet route:
     *
     * - When dealing with a full-mask interface address (i.e /32 or /128)
     * - When the port associated to the interface is declared as LOOPBACK
     */
    bool subnetOverlap = false, ip2meOverlap = false, skipSubnet = false;

    if (ifSubnetRoute == ifIp2meRoute || port.m_type == Port::LOOPBACK)
    {
        skipSubnet = true;
    }

    if (!skipSubnet)
    {
        subnetOverlap = trackIntfRouteOverlap(ifSubnetRoute);
    }
    ip2meOverlap = trackIntfRouteOverlap(ifIp2meRoute);

    /* Based on above results, proceed to create routes identified as unique. */
    if (subnetOverlap)
    {
        if (!ip2meOverlap)
        {
            addIp2MeRoute(ifIp2meRoute.prefix);
        }
    }
    else /* !subnetOverlap */
    {
        if (!skipSubnet)
        {
            if (!ip2meOverlap)
            {
                addSubnetRoute(port, ifSubnetRoute.prefix);
                addIp2MeRoute(ifIp2meRoute.prefix);
            }
            else
            {
                addSubnetRoute(port, ifSubnetRoute.prefix);
            }
        }
        else
        {
            if (!ip2meOverlap)
            {
                addIp2MeRoute(ifIp2meRoute.prefix);
            }
        }
    }
}

/*
 * Method's goal is to track/record any potential overlap between the interfaces
 * configured in the system, and alert caller of such an incident.
 */
bool IntfsOrch::trackIntfRouteOverlap(const IntfRouteEntry &ifRoute)
{
    SWSS_LOG_ENTER();

    string ifRouteStr = ifRoute.prefix.to_string();

    auto iterIfRoute = m_intfRoutes.find(ifRouteStr);
    if (iterIfRoute == m_intfRoutes.end())
    {
        m_intfRoutes[ifRouteStr].push_back(ifRoute);
        return false;
    }

    auto listIfRoutes = iterIfRoute->second;

    for (auto &curIfRoute : listIfRoutes)
    {
        if (curIfRoute.prefix == ifRoute.prefix)
        {
            SWSS_LOG_WARN("New %s route %s for interface %s overlaps with "
                          "existing route %s for interface %s. "
                          "Skipping...",
                          ifRoute.type.c_str(),
                          ifRouteStr.c_str(),
                          ifRoute.ifName.c_str(),
                          curIfRoute.prefix.to_string().c_str(),
                          curIfRoute.ifName.c_str());

            iterIfRoute->second.push_back(ifRoute);
            return true;
        }
    }

    return false;
}

void IntfsOrch::deleteIntfRoutes(const IntfRouteEntry &ifRoute,
                                 const Port           &port)
{
    SWSS_LOG_ENTER();

    IntfRouteEntry ifSubnetRoute(ifRoute.prefix.getSubnet(),
                                 ifRoute.ifName,
                                 "subnet");
    IntfRouteEntry ifIp2meRoute(getIp2mePrefix(ifRoute.prefix),
                                ifRoute.ifName,
                                "ip2me");

    /*
     * As we did for route creation case, we will skip the deletion of the subnet
     * route in two scenarios:
     *
     * - When dealing with a full-mask interface address (i.e /32 or /128)
     * - When the port associated to the interface is declared as LOOPBACK
     */
    bool skipSubnet = false;
    if (ifSubnetRoute == ifIp2meRoute || port.m_type == Port::LOOPBACK)
    {
        skipSubnet = true;
    }

    if (!skipSubnet)
    {
        deleteIntfRoute(ifSubnetRoute, port);
    }
    deleteIntfRoute(ifIp2meRoute, port);
}

void IntfsOrch::deleteIntfRoute(const IntfRouteEntry &ifRoute, const Port &port)
{
    SWSS_LOG_ENTER();

    string ifRouteStr = ifRoute.prefix.to_string();

    /* Return if there's no matching ifRoute in the map */
    if (!m_intfRoutes.count(ifRouteStr))
    {
        return;
    }

    /*
     * Obtain the list of routeEntries associated to this ifRoute, and iterate
     * through it looking for the matching entry (x) to eliminate. We are dealing
     * with two cases here:
     *
     * 1) If (x) is at the front of the list, then (x) is the 'active' route,
     * meaning the one that got pushed down to hw. In this case we will need to
     * 'resurrect' other (if any) overlapping routeEntry.
     *
     * 2) If (x) is at any other position in the list, then we will simply
     * eliminate it from the global hashmap, as there's no notion of this route
     * anywhere else.
     */
    auto list = m_intfRoutes[ifRouteStr];

    for (auto it = list.begin(); it != list.end(); ++it)
    {
        if (it->ifName == ifRoute.ifName)
        {
            /* Case 1) */
            if (it == list.begin())
            {
                SWSS_LOG_NOTICE("Eliminating active %s route %s from "
                                "interface %s",
                                it->type.c_str(),
                                it->prefix.to_string().c_str(),
                                it->ifName.c_str());

                if (it->type == "subnet")
                {
                    removeSubnetRoute(port, ifRoute.prefix);
                }
                else if (it->type == "ip2me")
                {
                    removeIp2MeRoute(ifRoute.prefix);
                }

                /*
                 * Notice that the resurrection-order is vital here. We must
                 * necessarily pick the oldest entry in the list (next element),
                 * in order to keep full consistency with kernel's tie-breaking
                 * logic.
                 */
                auto itNext = next(it);
                if (itNext != list.end())
                {
                    resurrectIntfRoute(*itNext);
                }
                list.pop_front();
            }
            /* Case 2) */
            else
            {
                SWSS_LOG_NOTICE("Eliminating overlapped %s route %s from "
                                "interface %s",
                                it->type.c_str(),
                                it->prefix.to_string().c_str(),
                                it->ifName.c_str());
                list.erase(it);
            }

            m_intfRoutes[ifRouteStr] = list;

            if (!list.size())
            {
                m_intfRoutes.erase(ifRouteStr);
            }

            break;
        }
    }
}

void IntfsOrch::resurrectIntfRoute(const IntfRouteEntry &ifRoute)
{
    SWSS_LOG_ENTER();

    /* Obtain intf's associated port */
    Port port;
    if (!gPortsOrch->getPort(ifRoute.ifName, port))
    {
        SWSS_LOG_NOTICE("Missing port associated to ip-address %s being "
                        "resurrected on interface %s ",
                        ifRoute.prefix.to_string().c_str(),
                        ifRoute.ifName.c_str());
        return;
    }

    SWSS_LOG_NOTICE("Resurrecting overlapped %s route %s from interface %s ",
                    ifRoute.type.c_str(),
                    ifRoute.prefix.to_string().c_str(),
                    ifRoute.ifName.c_str());


    /* Kicking off resurrection process */
    if (ifRoute.type == "subnet")
    {
        addSubnetRoute(port, ifRoute.prefix);
        m_syncdIntfses[ifRoute.ifName].ip_addresses.insert(ifRoute.prefix);
    }
    else if (ifRoute.type == "ip2me")
    {
        addIp2MeRoute(ifRoute.prefix);
    }
}

/*
 * Perhaps to be moved to a more appropriate location (e.g. IpPrefix class).
 */
IpPrefix IntfsOrch::getIp2mePrefix(const IpPrefix &ip_prefix)
{
    string newRoutePrefixStr = ip_prefix.isV4() ?
        ip_prefix.getIp().to_string() + "/32" :
        ip_prefix.getIp().to_string() + "/128";

    return (IpPrefix(newRoutePrefixStr));
}
