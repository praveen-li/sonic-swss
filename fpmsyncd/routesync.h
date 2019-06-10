#ifndef __ROUTESYNC__
#define __ROUTESYNC__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include "warmRestartHelper.h"


namespace swss {

class RouteSync : public NetMsg
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    RouteSync(RedisPipeline *pipeline);

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    WarmStartHelper  m_warmStartHelper;

private:
    ProducerStateTable  m_routeTable;
    struct nl_cache    *m_link_cache;
    struct nl_sock     *m_nl_sock;
};

}

#endif
