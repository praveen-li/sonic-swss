#ifndef __INTFSYNC__
#define __INTFSYNC__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include <string>

#define STATEDB_TABLE_NAME_SEPARATOR CONFIGDB_TABLE_NAME_SEPARATOR
#define DUMMY_INTF_NAME     "dummy"
#define USB_INTF_NAME       "usb0"
#define VLAN_PREFIX         "Vlan"
#define LAG_PREFIX          "PortChannel"
#define PORT_PREFIX         "Ethernet"

namespace swss {

class IntfSync : public NetMsg
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    IntfSync(DBConnector *appDb, DBConnector *stateDb);

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

private:
    ProducerStateTable m_intfTable;
    Table m_statePortTable, m_stateLagTable, m_stateVlanTable;;
    bool isIntfStateOk(const std::string &alias);
};

}

#endif
