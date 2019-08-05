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
#include "routeorch.h"
#include "crmorch.h"
#include "bufferorch.h"
#include "directory.h"
#include "vnetorch.h"

extern sai_object_id_t gVirtualRouterId;
extern Directory<Orch*> gDirectory;

extern sai_router_interface_api_t*  sai_router_intfs_api;
extern sai_route_api_t*             sai_route_api;
extern sai_neighbor_api_t*          sai_neighbor_api;

extern sai_object_id_t gSwitchId;
extern PortsOrch *gPortsOrch;
extern RouteOrch *gRouteOrch;
extern CrmOrch *gCrmOrch;
extern BufferOrch *gBufferOrch;

const int intfsorch_pri = 35;

IntfsOrch::IntfsOrch(DBConnector *db, string tableName, VRFOrch *vrf_orch) :
        Orch(db, tableName, intfsorch_pri), m_vrfOrch(vrf_orch)
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

bool IntfsOrch::setRouterIntfsMtu(Port &port)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
    attr.value.u32 = port.m_mtu;

    sai_status_t status = sai_router_intfs_api->
            set_router_interface_attribute(port.m_rif_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set router interface %s MTU to %u, rv:%d",
                port.m_alias.c_str(), port.m_mtu, status);
        return false;
    }
    SWSS_LOG_NOTICE("Set router interface %s MTU to %u",
            port.m_alias.c_str(), port.m_mtu);
    return true;
}

set<IpPrefix> IntfsOrch:: getSubnetRoutes()
{
    SWSS_LOG_ENTER();

    set<IpPrefix> subnet_routes;

    for (auto it = m_syncdIntfses.begin(); it != m_syncdIntfses.end(); it++)
    {
        for (auto prefix : it->second.ip_addresses)
        {
            subnet_routes.emplace(prefix);
        }
    }

    return subnet_routes;
}

bool IntfsOrch::createIntf(Port            &port,
                           sai_object_id_t  vrf_id,
                           const IpPrefix  *ip_prefix)
{
    SWSS_LOG_ENTER();

    string alias = port.m_alias;

    auto it_intfs = m_syncdIntfses.find(alias);
    if (it_intfs == m_syncdIntfses.end())
    {
        if (alias != "lo")
        {
            if (!addRouterIntfs(vrf_id, port))
            {
                return false;
            }
        }
    }

    /*
     * Return here if no prefix is present or if we are dealing with a duplicated
     * address being added over the same interface.
     */
    if (!ip_prefix || m_syncdIntfses[alias].ip_addresses.count(*ip_prefix))
    {
        return true;
    }

    /*
     * TODO: Remove this overlap-prevention logic which only purposes is to
     * tackle 'ifconfig' special behavior. SONiC interface configuration is
     * now only possible through CLI/configDB, so there's no point in having
     * this code here. I'm not removing it myself due to the existence of
     * a few python UTs that are still relying on 'ifconfig' execution.
     *
     * NOTE: Overlap checking is required to handle ifconfig weird behavior.
     * When set IP address using ifconfig command it applies it in two stages.
     * On stage one it sets IP address with netmask /8. On stage two it
     * changes netmask to specified in command. As DB is async event to
     * add IP address with original netmask may come before event to
     * delete IP with netmask /8. To handle this we in case of overlap
     * we should wait until entry with /8 netmask will be removed.
     * Time frame between those event is quite small.*/
    bool overlaps = false;
    for (const auto &prefixIt: m_syncdIntfses[alias].ip_addresses)
    {
        if (prefixIt.isAddressInSubnet(ip_prefix->getIp()) ||
                ip_prefix->isAddressInSubnet(prefixIt.getIp()))
        {
            overlaps = true;
            SWSS_LOG_NOTICE("Router interface %s IP %s overlaps with %s.",
                            port.m_alias.c_str(),
                    prefixIt.to_string().c_str(), ip_prefix->to_string().c_str());
            break;
        }
    }

    if (overlaps)
    {
        /* Overlap of IP address network */
        return false;
    }

    /* Creating intfRoutes associated to this interface being defined */
    createIntfRoutes(IntfRouteEntry(*ip_prefix, alias), port);

    m_syncdIntfses[alias].ip_addresses.insert(*ip_prefix);

    return true;
}

bool IntfsOrch::deleteIntf(Port            &port,
                           sai_object_id_t  vrf_id,
                           const IpPrefix  *ip_prefix)
{
    SWSS_LOG_ENTER();

    string alias = port.m_alias;

    if (m_syncdIntfses.find(alias) != m_syncdIntfses.end())
    {
        if (m_syncdIntfses[alias].ip_addresses.count(*ip_prefix))
        {
            deleteIntfRoutes(IntfRouteEntry(*ip_prefix, alias), port);
            m_syncdIntfses[alias].ip_addresses.erase(*ip_prefix);
        }

        /* Remove router interface that no IP addresses are associated with */
        if (m_syncdIntfses[alias].ip_addresses.size() == 0)
        {
            if (alias != "lo")
            {
                if (!removeRouterIntfs(port))
                {
                    return false;
                }
            }

            m_syncdIntfses.erase(alias);
        }
    }

    return true;
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
            addIp2MeRoute(port.m_vr_id, ifIp2meRoute.prefix);
        }
    }
    else /* !subnetOverlap */
    {
        if (!skipSubnet)
        {
            if (!ip2meOverlap)
            {
                addSubnetRoute(port, ifSubnetRoute.prefix);
                addIp2MeRoute(port.m_vr_id, ifIp2meRoute.prefix);
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
                addIp2MeRoute(port.m_vr_id, ifIp2meRoute.prefix);
            }
        }
    }

    /*
     * A directed-broadcast route is expected in vlan-ipv4 scenarios where the
     * subnet-length of the associated interface-address is shorter than 30 bits.
     * If these conditions are met, and there's no overlap with an existing
     * interface, proceed to create a bcast route.
     */
    if (port.m_type == Port::VLAN &&
        ifRoute.prefix.isV4() &&
        ifRoute.prefix.getMaskLength() <= 30)
    {
        IntfRouteEntry ifBcastRoute(getBcastPrefix(ifRoute.prefix),
                                    ifRoute.ifName,
                                    "bcast");

        bool bcastOverlap = trackIntfRouteOverlap(ifBcastRoute);
        if (!bcastOverlap)
        {
            addDirectedBroadcast(port, ifBcastRoute.prefix);
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
            SWSS_LOG_ERROR("New %s route %s for interface %s overlaps with "
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

    /*
     * Remove directed-bcast route if applicable. See addIntfRoutes() case for
     * more details.
     */
    if (port.m_type == Port::VLAN &&
        ifRoute.prefix.isV4() &&
        ifRoute.prefix.getMaskLength() <= 30)
    {
        IntfRouteEntry ifBcastRoute(getBcastPrefix(ifRoute.prefix),
                                    ifRoute.ifName,
                                    "bcast");

        deleteIntfRoute(ifBcastRoute, port);
    }
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
                    removeIp2MeRoute(port.m_vr_id, ifRoute.prefix);
                }
                else if (it->type == "bcast")
                {
                    removeDirectedBroadcast(port, ifRoute.prefix);
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
    }
    else if (ifRoute.type == "ip2me")
    {
        addIp2MeRoute(port.m_vr_id, ifRoute.prefix);
    }
    else if (ifRoute.type == "bcast")
    {
        addDirectedBroadcast(port, ifRoute.prefix);
    }
}

void IntfsOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->isPortReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        vector<string> keys = tokenize(kfvKey(t), ':');
        string alias(keys[0]);
        IpPrefix ip_prefix;
        bool ip_prefix_in_key = false;

        if (keys.size() > 1)
        {
            ip_prefix = kfvKey(t).substr(kfvKey(t).find(':')+1);
            ip_prefix_in_key = true;
        }

        const vector<FieldValueTuple>& data = kfvFieldsValues(t);
        string vrf_name = "", vnet_name = "";

        for (auto idx : data)
        {
            const auto &field = fvField(idx);
            const auto &value = fvValue(idx);
            if (field == "vrf_name")
            {
                vrf_name = value;
            }
            else if (field == "vnet_name")
            {
                vnet_name = value;
            }
        }

        if (alias == "eth0" || alias == "docker0")
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        sai_object_id_t vrf_id = gVirtualRouterId;
        if (!vnet_name.empty())
        {
            VNetOrch* vnet_orch = gDirectory.get<VNetOrch*>();
            if (!vnet_orch->isVnetExists(vnet_name))
            {
                it++;
                continue;
            }
            vrf_id = vnet_orch->getVRid(vnet_name);
        }
        else if (!vrf_name.empty())
        {
            if (m_vrfOrch->isVRFexists(vrf_name))
            {
                it++;
                continue;
            }
            vrf_id = m_vrfOrch->getVRFid(vrf_name);
        }

        string op = kfvOp(t);

        SWSS_LOG_DEBUG("Interface %s ip %s request with type %s is received",
          alias.c_str(), ip_prefix.to_string().c_str(), op.c_str());

        if (op == SET_COMMAND)
        {
            if (alias == "lo")
            {
                if (!ip_prefix_in_key)
                {
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                Port port = Port(alias, Port::LOOPBACK, vrf_id);
                if (!createIntf(port,
                                vrf_id,
                                ip_prefix_in_key ? &ip_prefix : nullptr))
                {
                    it++;
                    continue;
                }

                it = consumer.m_toSync.erase(it);
                continue;
            }

            Port port;
            if (!gPortsOrch->getPort(alias, port))
            {
                /* TODO: Resolve the dependency relationship and add ref_count to port */
                it++;
                continue;
            }


            if (!createIntf(port, vrf_id, ip_prefix_in_key ? &ip_prefix : nullptr))
	    {
                it++;
                continue;
            }

	    it = consumer.m_toSync.erase(it);
        }

        else if (op == DEL_COMMAND)
        {
            if (alias == "lo")
            {
                Port port = Port(alias, Port::LOOPBACK, vrf_id);
                if (!deleteIntf(port, vrf_id, &ip_prefix))
                {
                    it++;
                    continue;
                }

                it = consumer.m_toSync.erase(it);
                continue;
            }

            Port port;
            /* Cannot locate interface */
            if (!gPortsOrch->getPort(alias, port))
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }

            vrf_id = port.m_vr_id;

            if (!deleteIntf(port, vrf_id, &ip_prefix))
            {
                it++;
                continue;
            }

            it = consumer.m_toSync.erase(it);
        }
    }
}

bool IntfsOrch::addRouterIntfs(sai_object_id_t vrf_id, Port &port)
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
    attr.value.oid = vrf_id;
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
        SWSS_LOG_ERROR("Failed to create router interface %s, rv:%d",
                port.m_alias.c_str(), status);
        throw runtime_error("Failed to create router interface.");
    }

    port.m_vr_id = vrf_id;

    gPortsOrch->setPort(port.m_alias, port);

    SWSS_LOG_NOTICE("Create router interface %s MTU %u", port.m_alias.c_str(), port.m_mtu);

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
    port.m_vr_id = 0;
    gPortsOrch->setPort(port.m_alias, port);

    SWSS_LOG_NOTICE("Remove router interface for port %s", port.m_alias.c_str());

    return true;
}

void IntfsOrch::addSubnetRoute(const Port &port, const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = port.m_vr_id;
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

    gRouteOrch->notifyNextHopChangeObservers(ip_prefix, IpAddresses(), true);
}

void IntfsOrch::removeSubnetRoute(const Port &port, const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = port.m_vr_id;
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

    gRouteOrch->notifyNextHopChangeObservers(ip_prefix, IpAddresses(), false);
}

void IntfsOrch::addIp2MeRoute(sai_object_id_t vrf_id, const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = vrf_id;
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

void IntfsOrch::removeIp2MeRoute(sai_object_id_t vrf_id, const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = vrf_id;
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

void IntfsOrch::addDirectedBroadcast(const Port &port, const IpPrefix &ip_prefix)
{
    sai_status_t status;
    sai_neighbor_entry_t neighbor_entry;
    IpAddress ip_addr;

    /* Return if not an IPv4 subnet */
    if (!ip_prefix.isV4())
    {
      return;
    }
    ip_addr = ip_prefix.getIp();

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

void IntfsOrch::removeDirectedBroadcast(const Port &port, const IpPrefix &ip_prefix)
{
    sai_status_t status;
    sai_neighbor_entry_t neighbor_entry;
    IpAddress ip_addr;

    /* Return if not an IPv4 subnet */
    if (!ip_prefix.isV4())
    {
        return;
    }
    ip_addr = ip_prefix.getIp();

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

/*
 * Helper functions. Perhaps to be moved to a more appropriate location (e.g.
 * IpPrefix class).
 */
IpPrefix IntfsOrch::getIp2mePrefix(const IpPrefix &ip_prefix)
{
    string newRoutePrefixStr = ip_prefix.isV4() ?
        ip_prefix.getIp().to_string() + "/32" :
        ip_prefix.getIp().to_string() + "/128";

    return (IpPrefix(newRoutePrefixStr));
}

IpPrefix IntfsOrch::getBcastPrefix(const IpPrefix &ip_prefix)
{
    string newRoutePrefixStr = ip_prefix.isV4() ?
        ip_prefix.getBroadcastIp().to_string() + "/32" :
        ip_prefix.getBroadcastIp().to_string() + "/128";

    return (IpPrefix(newRoutePrefixStr));
}
