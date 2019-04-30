#include <string.h>
#include <errno.h>
#include <system_error>
#include <netlink/route/link.h>
#include <netlink/route/addr.h>
#include "logger.h"
#include "netmsg.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "linkcache.h"
#include "intfsyncd/intfsync.h"

using namespace std;
using namespace swss;

IntfSync::IntfSync(DBConnector *appDb, DBConnector *stateDb) :
    m_intfTable(appDb, APP_INTF_TABLE_NAME),
    m_statePortTable(stateDb, STATE_PORT_TABLE_NAME, STATEDB_TABLE_NAME_SEPARATOR),
    m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME, STATEDB_TABLE_NAME_SEPARATOR),
    m_stateVlanTable(stateDb, STATE_VLAN_TABLE_NAME, STATEDB_TABLE_NAME_SEPARATOR)
{
}

bool IntfSync::isIntfStateOk(const string &alias)
{
    vector<FieldValueTuple> temp;

    if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
    {
        if (m_stateVlanTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Vlan %s is ready", alias.c_str());
            return true;
        }
    }
    else if (!alias.compare(0, strlen(PORT_PREFIX), PORT_PREFIX))
    {
        if (m_statePortTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Port %s is ready", alias.c_str());
            return true;
        }
    }
    else
    {
        SWSS_LOG_DEBUG("Special Port %s is always considered as ready", alias.c_str());
        return true;
    }

    SWSS_LOG_DEBUG("Interface %s is not ready", alias.c_str());
    return false;
}

void IntfSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    char addrStr[MAX_ADDR_SIZE + 1] = {0};
    struct rtnl_addr *addr = (struct rtnl_addr *)obj;
    string key;
    string scope = "global";
    string family;

    if ((nlmsg_type != RTM_NEWADDR) && (nlmsg_type != RTM_GETADDR) &&
        (nlmsg_type != RTM_DELADDR))
        return;

    /* Don't sync local routes */
    if (rtnl_addr_get_scope(addr) != RT_SCOPE_UNIVERSE)
    {
        scope = "local";
    }

    if (rtnl_addr_get_family(addr) == AF_INET)
        family = IPV4_NAME;
    else if (rtnl_addr_get_family(addr) == AF_INET6)
        family = IPV6_NAME;
    else
        // Not supported
        return;

    key = LinkCache::getInstance().ifindexToName(rtnl_addr_get_ifindex(addr));

    nl_addr2str(rtnl_addr_get_local(addr), addrStr, MAX_ADDR_SIZE);

    string msg_type_str;
    switch (nlmsg_type)
    {
        case RTM_GETADDR:
	    msg_type_str = "GET_ADDR";
	    break;
        case RTM_NEWADDR:
	    msg_type_str = "NEW_ADDR";
	    break;
        case RTM_DELADDR:
	    msg_type_str = "DEL_ADDR";
	    break;
        default:
	    msg_type_str = "UNKNOWN";
	    break;
    }

    SWSS_LOG_DEBUG("Interface %s:%s netlink with type %s is received",
      key.c_str(), addrStr, msg_type_str.c_str());

    /*
     * interface IP addresses on special interfaces (dummy, usb0 etc) are ignored
     * Otherwise, the IPs (e,g, link-local) on the interfaces would be handled uneccessarily
     */
    if (key == DUMMY_INTF_NAME || key == USB_INTF_NAME)
    {
        SWSS_LOG_NOTICE("IP: %s on interface: %s is ignored", addrStr, key.c_str());
        return;
    }

    /*
     * If interface is not ready, we skip the netlink messages
     * This could happen if we reload config and get netlink messages
     * from old kernel interfaces
     */
    if (!isIntfStateOk(key))
    {
        SWSS_LOG_NOTICE("Interface %s with ip %s is not ready, netlink type %s"
	  " is received and skipped", key.c_str(), addrStr, msg_type_str.c_str());
	return;
    }

    key+= ":";
    key+= addrStr;
    if (nlmsg_type == RTM_DELADDR)
    {
        m_intfTable.del(key);
        return;
    }

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple f("family", family);
    FieldValueTuple s("scope", scope);
    fvVector.push_back(s);
    fvVector.push_back(f);
    m_intfTable.set(key, fvVector);
}
