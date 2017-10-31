#include <iostream>
#include <iomanip>
#include <vector>
#include <functional>
#include <algorithm>
#include <unistd.h>
#include <net/if.h>
#include <cerrno>
#include <cstring>
#include "schema.h"
#include "exec.h"
#include "cfgmgr.h"
#include "intfcfgmgr.h"

#include "../../orchagent/orchbase.h"

using namespace std;
using namespace swss;

const string INTFS_PREFIX = "Ethernet";
const string VLAN_PREFIX = "Vlan";
const string LAG_PREFIX = "PortChannel";

[[ noreturn ]] static void usage(std::string program, int status, std::string message)
{
    if (message.size() != 0)
    {
        std::cout << message << std::endl << std::endl;
    }
    std::cout << "Usage:  " << program << " intf { add | del } PREFIX  dev IFNAME\n" << std::endl
              << "\t" << program << " intf show <config | state> [ dev IFNAME ]" << std::endl;

    exit(status);
}

int IntfCfgMgr::intf_modify(Operation cmd, int argc, char **argv)
{
    char *dev = NULL;
    string key;
    string prefix;
    string scope = "global";
    string family = IPV4_NAME;
    Table  *table;

    if (argc <= 0) {
        usage("cfgmgr", -1, "Invalid option");
    }

    // TODO: more validity check on prefix.
    prefix = *argv;

    NEXT_ARG();
    while (argc > 0) {
        if (matches(*argv, "dev") == 0) {
            NEXT_ARG();
            dev = *argv;
        } else {
            if (matches(*argv, "help") == 0) {
                usage("cfgmgr", 0, "");
            }
        }
        argc--; argv++;
    }

    if (dev == NULL) {
        cout << "dev IFNAME is required arguments" << endl;
        usage("cfgmgr", -1, "Invalid option");
    }

    if (if_nametoindex(dev) == 0) {
        cout << "Cannot find device " << dev << " : " << std::strerror(errno) << endl;
        return -1;
    }

    key = dev;
    key += CONFIGDB_KEY_SEPARATOR;
    key += prefix;

    if (!key.compare(0, INTFS_PREFIX.length(), INTFS_PREFIX))
    {
        table = &m_cfgIntfTable;
    }
    else if (!key.compare(0, LAG_PREFIX.length(), LAG_PREFIX))
    {
        table = &m_cfgLagIntfTable;
    }
    else if (!key.compare(0, VLAN_PREFIX.length(), VLAN_PREFIX))
    {
        table = &m_cfgVlanIntfTable;
    }
    else
    {
        cout << dev << " is not a Ethernet or PortChannel or Vlan interface, not supported for now" << endl;
        return -1;
    }

    if (cmd == IntfCfgMgr::DELETE)
    {
        table->del(key);
    }
    else
    {
        std::vector<FieldValueTuple> fvVector;
        FieldValueTuple f("family", family);
        FieldValueTuple s("scope", scope);
        fvVector.push_back(s);
        fvVector.push_back(f);
        table->set(key, fvVector);
    }
    return 0;
}

int IntfCfgMgr::intf_show(int argc, char **argv)
{
    char *filter_dev = NULL;
    unsigned int if_index;
    string cmd = "ip address show ";
    string redis_cmd_db = "redis-cli -n ";
    string redis_cmd_keys = "\\*INTERFACE\\|\\*";
    string redis_cmd;
    ShowOpEnum show_op = SHOW_NONE;
    short vid = -1;

    while (argc > 0) {
        if (matches(*argv, "dev") == 0) {
            NEXT_ARG();
            filter_dev = *argv;
        }
        else if (matches(*argv, "config") == 0) {
            show_op = SHOW_CONFIG;
        }
        else if (matches(*argv, "state") == 0) {
            show_op = SHOW_STATE;
        }
        argc--; argv++;
    }

    if (show_op == SHOW_NONE)
    {
        usage("cfgmgr", 0, "");
    }

    redis_cmd_db += std::to_string(CONFIG_DB) + " ";

    if (vid >= 0) {
        redis_cmd_keys += std::to_string(vid) + "\\*";
    }

    if (filter_dev) {
        if_index = if_nametoindex(filter_dev);
        if (if_index == 0) {
            cout << "Cannot find bridge device " << filter_dev << " : " << std::strerror(errno) << endl;
            return -1;
        }
        cmd += " dev ";
        cmd += filter_dev;

        redis_cmd_keys += filter_dev;
        redis_cmd_keys += "\\*";
    }

    redis_cmd = redis_cmd_db + " KEYS " + redis_cmd_keys;
    redis_cmd += " | xargs -n 1  -I %   sh -c 'echo \"%\"; ";
    redis_cmd += redis_cmd_db + "hgetall \"%\" | paste -d '='  - - | sed  's/^/$/'; echo'";

    string res;
    if (show_op == SHOW_CONFIG)
    {
        cout << "-----Redis ConfigDB data---" << endl;
        //cout << redis_cmd <<endl;

        swss::exec(redis_cmd, res);
        cout << res;
        return 0;
    }

    cout << "----Linux hostenv data----" << endl;
    swss::exec(cmd, res);
    cout << res << endl;
    return 0;
}

IntfCfgMgr::IntfCfgMgr(DBConnector *db) :
    m_cfgIntfTable(db, CFG_INTF_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
    m_cfgLagIntfTable(db, CFG_LAG_INTF_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
    m_cfgVlanIntfTable(db, CFG_VLAN_INTF_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR)
{

}

int do_intf(int argc, char **argv)
{
    auto exitWithUsage = std::bind(usage, "cfgmgr", std::placeholders::_1, std::placeholders::_2);

    DBConnector db(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    IntfCfgMgr cfgmgr(&db);

    if (argc > 0)
    {
        if (matches(*argv, "add") == 0)
            return cfgmgr.intf_modify(IntfCfgMgr::ADD, argc-1, argv+1);
        if (matches(*argv, "delete") == 0)
            return cfgmgr.intf_modify(IntfCfgMgr::DELETE, argc-1, argv+1);
        if (matches(*argv, "show") == 0)
            return cfgmgr.intf_show(argc-1, argv+1);
        if (matches(*argv, "help") == 0)
            exitWithUsage(EXIT_SUCCESS, "");
        else
            exitWithUsage(EXIT_FAILURE, "Invalid option");
    }
    else {
        exitWithUsage(EXIT_FAILURE, "Invalid option");
    }

    return EXIT_SUCCESS;
}
