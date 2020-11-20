from swsscommon import swsscommon
import redis
import time
import os
import pytest
from pytest import *
import json
import re
from port_dpb import Port
from port_dpb import DPB

@pytest.mark.usefixtures('dpb_setup_fixture')
@pytest.mark.usefixtures('dvs_acl_manager')
@pytest.mark.usefixtures('dvs_vlan_manager')
class TestPortDPBSystem(object):

    def verify_only_ports_exist(self, dvs, port_names):
        all_port_names = ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"]
        for port_name in all_port_names:
            p = Port(dvs, port_name)
            if port_name in port_names:
                assert(p.exists_in_config_db() == True)
                assert(p.exists_in_app_db() == True)
                assert(p.exists_in_asic_db() == True)
            else:
                assert(p.exists_in_config_db() == False)
                assert(p.exists_in_app_db() == False)
                assert(p.exists_in_asic_db() == False)

    '''
    |-----------------------------------------------------------------------------------------------------
    |                   | 1X40G | 1X100G | 4X10G | 4X25G | 2X50G | 2x25G(2)+1x50G(2) | 1x50G(2)+2x25G(2) |
    |-----------------------------------------------------------------------------------------------------
    | 1X40G             |  NA   |        |       |       |       |                   |                   |
    |-----------------------------------------------------------------------------------------------------
    | 1X100G            |       |   NA   |       |  P    |  P    |        P          |        P          |
    |-----------------------------------------------------------------------------------------------------
    | 4X10G             |       |        |  NA   |       |       |                   |                   |
    |-----------------------------------------------------------------------------------------------------
    | 4X25G             |       |   P    |       |  NA   |  P    |        P          |        P          |
    |-----------------------------------------------------------------------------------------------------
    | 2X50G             |       |   P    |       |  P    |  NA   |        P          |        P          |
    |-----------------------------------------------------------------------------------------------------
    | 2x25G(2)+1x50G(2) |       |   P    |       |  P    |  P    |        NA         |        P          |
    |-----------------------------------------------------------------------------------------------------
    | 1x50G(2)+2x25G(2) |       |   P    |       |  P    |  P    |        P          |        NA         |
    |-----------------------------------------------------------------------------------------------------

    NA    --> Not Applicable
    P     --> Pass
    F     --> Fail
    Empty --> Not Tested
    '''
    @pytest.mark.parametrize('root_port, breakout_mode',  [
        ('Ethernet0', '2x50G'),
        ('Ethernet0', '4x25G[10G]'),
        ('Ethernet0', '2x50G'),
        ('Ethernet0', '2x25G(2)+1x50G(2)'),
        ('Ethernet0', '2x50G'),
        ('Ethernet0', '1x50G(2)+2x25G(2)'),
        ('Ethernet0', '2x50G'),
        ('Ethernet0', '1x100G[40G]'),
        ('Ethernet0', '4x25G[10G]'),
        ('Ethernet0', '2x25G(2)+1x50G(2)'),
        ('Ethernet0', '4x25G[10G]'),
        ('Ethernet0', '1x50G(2)+2x25G(2)'),
        ('Ethernet0', '4x25G[10G]'),
        ('Ethernet0', '1x100G[40G]'),
        ('Ethernet0', '2x25G(2)+1x50G(2)'),
        ('Ethernet0', '1x50G(2)+2x25G(2)'),
        ('Ethernet0', '2x25G(2)+1x50G(2)'),
        ('Ethernet0', '1x100G[40G]'),
        ('Ethernet0', '1x50G(2)+2x25G(2)'),
        ('Ethernet0', '1x100G[40G]')
    ], scope="function")
    def test_port_breakout_simple(self, dvs, root_port, breakout_mode):
        dvs.setup_db()
        dpb = DPB()

        dvs.change_port_breakout_mode(root_port, breakout_mode)
        dvs.verify_port_breakout_mode(root_port, breakout_mode)
        expected_ports = dpb.get_child_ports(root_port, breakout_mode)
        self.verify_only_ports_exist(dvs, expected_ports)

    def test_port_breakout_with_vlan(self, dvs):
        dvs.setup_db()
        portName = "Ethernet0"
        vlanID = "100"
        breakoutMode1 = "1x100G[40G]"
        breakoutMode2 = "4x25G[10G]"
        breakoutOption = "-f" #Force breakout by deleting dependencies

        # Create VLAN
        self.dvs_vlan.create_vlan(vlanID)

        # Verify VLAN is created
        self.dvs_vlan.get_and_verify_vlan_ids(1)

        # Add port to VLAN
        self.dvs_vlan.create_vlan_member(vlanID, portName)

        # Verify VLAN member is created
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)

        # Breakout port from 1x100G[40G] --> 4x25G[10G]
        dvs.verify_port_breakout_mode("Ethernet0", breakoutMode1)
        dvs.change_port_breakout_mode("Ethernet0", breakoutMode2, breakoutOption)

        # Verify DPB is successful
        dvs.verify_port_breakout_mode("Ethernet0", breakoutMode2)

        # Verify port is removed from VLAN
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        # Delete VLAN
        self.dvs_vlan.remove_vlan(vlanID)

        # Verify VLAN is deleted
        self.dvs_vlan.get_and_verify_vlan_ids(0)

        # Breakout port from 4x25G[10G] --> 1x100G[40G]
        dvs.change_port_breakout_mode("Ethernet0", breakoutMode1)

        # Verify DPB is successful
        dvs.verify_port_breakout_mode("Ethernet0", breakoutMode1)

    def test_port_breakout_with_acl(self, dvs):
        dvs.setup_db()
        dpb = DPB()

        # Create ACL table "test" and bind it to Ethernet0
        bind_ports = ["Ethernet0"]
        self.dvs_acl.create_acl_table("test", "L3", bind_ports)

        # Verify ACL teable is created
        self.dvs_acl.verify_acl_table_count(1)

        # Verify that ACL group OID is created.
        # Just FYI: Usually one ACL group OID is created per port,
        #           even when port is bound to multiple ACL tables
        self.dvs_acl.verify_acl_group_num(1)

        # Verify that port is correctly bound to table by looking into
        # ACL member table, which binds ACL group OID of a port and
        # ACL table OID.
        acl_table_ids = self.dvs_acl.get_acl_table_ids()
        self.dvs_acl.verify_acl_table_ports_binding(bind_ports, acl_table_ids[0])

        # Verify current breakout mode, perform breakout without force dependency
        # delete option
        dvs.verify_port_breakout_mode("Ethernet0", "1x100G[40G]")
        dvs.change_port_breakout_mode("Ethernet0", "4x25G[10G]")

        # Verify that breakout did NOT succeed
        dvs.verify_port_breakout_mode("Ethernet0", "1x100G[40G]")

        # Do breakout with force option, and verify that it succeeds
        dvs.change_port_breakout_mode("Ethernet0", "4x25G[10G]", "-f")
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", "4x25G[10G]")

        # Verify port is removed from ACL table
        self.dvs_acl.verify_acl_table_count(1)
        self.dvs_acl.verify_acl_group_num(0)

        # Verify child ports are created.
        self.verify_only_ports_exist(dvs, ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"])

        # Move back to 1x100G[40G] mode and verify current mode
        dvs.change_port_breakout_mode("Ethernet0", "1x100G[40G]", "-f")
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", "1x100G[40G]")

        # Remove ACL table and verify the same
        self.dvs_acl.remove_acl_table("test")
        self.dvs_acl.verify_acl_table_count(0)

    @pytest.mark.skip("FALCO-2746")
    def test_cli_command_with_force_option(self, dvs):
        dvs.setup_db()
        dpb = DPB()
        self.setup_db(dvs);

        portGroup = ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"]
        rootPortName = portGroup[0]
        vlanID = "100"
        aclTableName = "DPB_ACL_TBL_1"
        breakoutMode1x = "1x100G[40G]"
        breakoutMode2x = "2x50G"
        breakoutMode4x = "4x25G[10G]"
        breakoutOption = "-f -v" #Force breakout by deleting dependencies

        # Breakout port with no dependency using "-f" option
        dvs.change_port_breakout_mode(rootPortName, breakoutMode4x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode1x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)

        # Breakout port with VLAN and ACL dependency

        # Create ACL table and bind port
        self.dvs_acl.verify_acl_group_num(0)
        bind_ports = []
        bind_ports.append(rootPortName)
        self.dvs_acl.create_acl_table(aclTableName, "L3", bind_ports)
        self.dvs_acl.verify_acl_group_num(1)

        # Create VLAN and add port to VLAN
        self.dvs_vlan.create_vlan(vlanID)
        self.dvs_vlan.create_vlan_member(vlanID, rootPortName)
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)

        # Breakout port and make sure it succeeds and associations are removed
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode4x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        self.dvs_acl.verify_acl_group_num(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        # Add ports to ACL and VLAN tables
        self.dvs_acl.update_acl_table(aclTableName, portGroup)
        for p in portGroup:
            self.dvs_vlan.create_vlan_member(vlanID, p)
        self.dvs_acl.verify_acl_group_num(len(portGroup))
        self.dvs_vlan.get_and_verify_vlan_member_ids(len(portGroup))

        # Breakout with "-f" option and ensure it succeeds and associations are removed
        dvs.change_port_breakout_mode(rootPortName, breakoutMode1x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        self.dvs_acl.verify_acl_group_num(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        # Cleanup

        # Remove ACL and VLAN tables
        self.dvs_acl.remove_acl_table(aclTableName)
        self.dvs_vlan.remove_vlan(vlanID)

        # Verify cleanup
        self.dvs_acl.verify_acl_table_count(0)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

        # check ASIC router interface database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface
        assert len(intf_entries) == 1

        # Bring up port
        self.set_admin_status(dvs, "Ethernet8", "up")

        # Create L3 interface
        self.create_l3_intf("Ethernet8", "");

        # Configure IPv4 address on Ethernet8
        self.add_ip_address("Ethernet8", "10.0.0.8/31")

        intf_entries = tbl.getKeys()
        # one loopback router interface and one port based router interface
        assert len(intf_entries) == 2

        # check ASIC route database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.0.0.8/31":
                subnet_found = True
            if route["dest"] == "10.0.0.8/32":
                ip2me_found = True

        assert subnet_found and ip2me_found

        # Breakout Ethernet8 WITH "-f" option and ensure cleanup happened
        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode1x)
        dvs.change_port_breakout_mode("Ethernet8", breakoutMode2x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode2x)

        # check ASIC router interface database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface
        assert len(intf_entries) == 1

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.0.0.8/31":
                assert False
            if route["dest"] == "10.0.0.8/32":
                assert False

        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode2x)
        dvs.change_port_breakout_mode("Ethernet8", breakoutMode1x)
        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode1x)

    @pytest.mark.skip("FALCO-2746")
    def test_cli_command_with_load_port_breakout_config_option(self, dvs):
        dvs.setup_db()
        dpb = DPB()
        self.setup_db(dvs);
        # Note below definitions are dependent on port_breakout_config_db.json
        # That is vlanIDs, aclTableNames are all should match with
        # VLANs and ACL tables in port_breakout_config_db.json
        portGroup = ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"]
        rootPortName = portGroup[0]
        vlanIDs = ["100", "101"]
        aclTableNames = ["DPB_ACL_TBL_1", "DPB_ACL_TBL_2"]
        breakoutMode1x = "1x100G[40G]"
        breakoutMode2x = "2x50G"
        breakoutMode4x = "4x25G[10G]"
        breakoutOption = "-l"

        # Lets create ACL and VLAN tables
        bind_ports = []
        for aclTableName in aclTableNames:
            self.dvs_acl.create_acl_table(aclTableName, "L3", bind_ports)
        for vlanID in vlanIDs:
            self.dvs_vlan.create_vlan(vlanID)

        # Breakout port and expect that newly created ports are
        # automatically added to VLANs and ACL tables as per
        # port_breakout_config_db.json
        self.dvs_acl.verify_acl_group_num(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode4x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        self.dvs_acl.verify_acl_group_num(len(portGroup))
        self.dvs_vlan.get_and_verify_vlan_member_ids(len(portGroup))

        # Breakout port and expect that all ports except root port
        # get removed from ACL and VLAN tables
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode1x, breakoutOption + " -f")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        self.dvs_acl.verify_acl_group_num(1)
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)

        # Breakout port with "-f" and WITHOUT "-l" and expect that
        # breakout succeeds and root port gets removed from
        # VLAN and ACL table
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode4x, "-f")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        self.dvs_acl.verify_acl_group_num(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        # Exercise port group spanned across different VLAN and ACl table
        portGroup = ["Ethernet4", "Ethernet5", "Ethernet6", "Ethernet7"]
        rootPortName = portGroup[0]
        breakoutMode2x = "2x50G"

        # Breakout port and expect that newly created ports are
        # automatically added to VLANs and ACL tables as per
        # port_breakout_config_db.json
        self.dvs_acl.verify_acl_group_num(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode4x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        self.dvs_acl.verify_acl_group_num(len(portGroup))
        self.dvs_vlan.get_and_verify_vlan_member_ids(len(portGroup))

        # Breakout port and expect that Ethernet4 and Ethernet6 remain in
        # ACL and VLAN where as Ethernet5 and Ethernet7 get removed from
        # VLAN and ACL tables
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode2x, breakoutOption + " -f")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode2x)
        self.dvs_acl.verify_acl_group_num(2)
        self.dvs_vlan.get_and_verify_vlan_member_ids(2)

        # Breakout again and verify that only root port (Ethernet4) remains in
        # in VLAN and ACL and Ethernet6 gets removed.
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode2x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode1x, breakoutOption + " -f")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        self.dvs_acl.verify_acl_group_num(1)
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)

        # Breakout port without "-l" option and ensure that root port
        # gets removed from VLAN and ACL
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode2x, "-f")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode2x)
        self.dvs_acl.verify_acl_group_num(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        #Cleanup

        # Move both Ethernet0 and Ethernet4 back to default mode
        dvs.change_port_breakout_mode("Ethernet0", breakoutMode1x)
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", breakoutMode1x)
        dvs.change_port_breakout_mode("Ethernet4", breakoutMode1x)
        dpb.verify_port_breakout_mode(dvs, "Ethernet4", breakoutMode1x)

        # Delete VLANs and ACL tables
        bind_ports = []
        for aclTableName in aclTableNames:
            self.dvs_acl.remove_acl_table(aclTableName)
        for vlanID in vlanIDs:
            self.dvs_vlan.remove_vlan(vlanID)

        # Verify cleanup
        self.dvs_acl.verify_acl_table_count(0)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

        ##### Interface dependency test ############

        # check ASIC router interface database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface
        assert len(intf_entries) == 1

        # Breakout Ethernet8 WITH "-l" option and ensure
        # ip address gets configured as per port_breakout_config_db.json
        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode1x)
        dvs.change_port_breakout_mode("Ethernet8", breakoutMode2x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode2x)

        intf_entries = tbl.getKeys()
        # one loopback router interface and one port based router interface
        assert len(intf_entries) == 2

        # check ASIC route database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.0.0.8/31":
                subnet_found = True
            if route["dest"] == "10.0.0.8/32":
                ip2me_found = True

        assert subnet_found and ip2me_found

        # Breakout Ethernet8 WITH "-f" option and ensure cleanup happened
        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode2x)
        dvs.change_port_breakout_mode("Ethernet8", breakoutMode1x, "-f")
        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode1x)

        # check ASIC router interface database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface
        assert len(intf_entries) == 1

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.0.0.8/31":
                assert False
            if route["dest"] == "10.0.0.8/32":
                assert False

    def test_cli_command_negative(self, dvs):
        dvs.setup_db()
        dpb = DPB()
        self.setup_db(dvs);

        portGroup = ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"]
        rootPortName = portGroup[0]
        vlanIDs = ["100", "101"]
        aclTableNames = ["DPB_ACL_TBL_1", "DPB_ACL_TBL_2"]
        breakoutMode1x = "1x100G[40G]"
        breakoutMode4x = "4x25G[10G]"

        # Create only one ACL table and one VLAN table
        bind_ports = []
        self.dvs_acl.create_acl_table(aclTableNames[0], "L3", bind_ports)
        self.dvs_vlan.create_vlan(vlanIDs[0])

        # Add root port to ACL and VLAN tables
        bind_ports = []
        bind_ports.append(rootPortName)
        self.dvs_acl.update_acl_table(aclTableNames[0], bind_ports)
        self.dvs_vlan.create_vlan_member(vlanIDs[0], rootPortName)

        # Breakout port WITHOUT "-f" option when dependencies exist
        # TBD: Verify the list of dependencies returned by CLI command
        self.dvs_acl.verify_acl_group_num(1)
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode4x)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        self.dvs_acl.verify_acl_group_num(1)
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)

        # Breakout port WITH "-f" option, and WITHOUT "-l" option
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode4x, "-f")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        self.dvs_acl.verify_acl_group_num(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        # Delete VLAN table, ensure breakout WITH "-l" fails
        self.dvs_vlan.remove_vlan(vlanIDs[0])
        self.dvs_vlan.get_and_verify_vlan_ids(0)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode1x, "-l")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        self.dvs_acl.verify_acl_group_num(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        # Delete ACL table, Add back VLAN table and
        # ensure breakout WITH "-l" fails
        self.dvs_acl.remove_acl_table(aclTableNames[0])
        self.dvs_acl.verify_acl_table_count(0)
        self.dvs_vlan.create_vlan(vlanIDs[0])
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode1x, "-l")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        self.dvs_acl.verify_acl_group_num(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        # Create both ACL tables (as per port_breakout_config_db.json,
        # Ethernet0 is in both ACL tables and one VLAN table)
        # and ensure, breakout succeeds
        bind_ports = []
        self.dvs_acl.create_acl_table(aclTableNames[0], "L3", bind_ports)
        self.dvs_acl.create_acl_table(aclTableNames[1], "L3", bind_ports)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode1x, "-l")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        self.dvs_acl.verify_acl_group_num(1)
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)

        # Delete ACL and VLAN tables
        self.dvs_vlan.remove_vlan_member(vlanIDs[0], rootPortName)
        self.dvs_vlan.remove_vlan(vlanIDs[0])
        self.dvs_acl.remove_acl_table(aclTableNames[0])
        self.dvs_acl.remove_acl_table(aclTableNames[1])

        # TBD: Provide "-l" option without port_breakout_config_db.json file

        # Verify cleanup
        self.dvs_acl.verify_acl_table_count(0)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

    def test_dpb_arp_flush(self, dvs):
        dvs.setup_db()
        self.setup_db(dvs);

        portName = "Ethernet0"
        vrfName = ""
        ipAddress = "10.0.0.0/31"
        srv0MAC = "00:00:00:00:01:11"

        self.clear_srv_config(dvs)

        # Create l3 interface
        rif_oid = self.create_l3_intf(portName, vrfName)

        # set ip address
        self.add_ip_address(portName, ipAddress)

        # bring up interface
        self.set_admin_status(dvs, portName, "up")

        # Set IP address and default route
        cmd = "ip link set eth0 address " + srv0MAC
        dvs.servers[0].runcmd(cmd)
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        # Get neighbor and ARP entry
        dvs.servers[0].runcmd("ping -c 1 10.0.0.0")

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        route = json.loads(intf_entries[0])
        assert route["ip"] == "10.0.0.1"
        assert route["rif"] == rif_oid
        (status, fvs) = tbl.get(intf_entries[0])
        assert status == True

        fvs_dict = dict(fvs)
        assert fvs_dict["SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS"] == srv0MAC

        # Breakout port and make sure NEIGHBOR entry is removed
        dvs.verify_port_breakout_mode("Ethernet0", "1x100G[40G]")
        dvs.change_port_breakout_mode("Ethernet0", "4x25G[10G]", "-f")
        dvs.verify_port_breakout_mode("Ethernet0", "4x25G[10G]")

        #Verify ARP/Neighbor entry is removed
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        dvs.change_port_breakout_mode("Ethernet0", "1x100G[40G]")
        dvs.verify_port_breakout_mode("Ethernet0", "1x100G[40G]")

    def test_dpb_vlan_arp_flush(self, dvs):
        dvs.setup_db()
        self.setup_db(dvs);

        self.clear_srv_config(dvs)
        vlanID = "100"
        portName = "Ethernet0"
        vlanName = "Vlan" + str(vlanID)
        vrfName = ""
        ipAddress = "10.0.0.0/31"
        srv0MAC = "00:00:00:00:01:11"

        self.create_vlan(vlanID)

        self.create_vlan_member(vlanID, portName)

        # bring up interface
        self.set_admin_status(dvs, portName, "up")
        self.set_admin_status(dvs, vlanName, "up")

        # create vlan interface
        rif_oid = self.create_l3_intf(vlanName, vrfName)

        # assign IP to interface
        self.add_ip_address(vlanName, ipAddress)

        # Set IP address and default route
        cmd = "ip link set eth0 address " + srv0MAC
        dvs.servers[0].runcmd(cmd)
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        # Get neighbor and ARP entry
        dvs.servers[0].runcmd("ping -c 1 10.0.0.0")

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        route = json.loads(intf_entries[0])
        assert route["ip"] == "10.0.0.1"
        assert route["rif"] == rif_oid
        (status, fvs) = tbl.get(intf_entries[0])
        assert status == True

        fvs_dict = dict(fvs)
        assert fvs_dict["SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS"] == srv0MAC

        # Breakout port and make sure NEIGHBOR entry is removed
        dvs.verify_port_breakout_mode("Ethernet0", "1x100G[40G]")
        dvs.change_port_breakout_mode("Ethernet0", "4x25G[10G]", "-f")
        dvs.verify_port_breakout_mode("Ethernet0", "4x25G[10G]")

        #Verify ARP/Neighbor entry is removed
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        dvs.change_port_breakout_mode("Ethernet0", "1x100G[40G]")
        dvs.verify_port_breakout_mode("Ethernet0", "1x100G[40G]")

        # Remove IP from interface, and then remove interface
        self.remove_ip_address(vlanName, ipAddress)
        self.remove_l3_intf(vlanName)

        # Remove VLAN(note that member was removed during port breakout)
        self.remove_vlan(vlanID)

    def test_arp_flush_on_port_oper_shut(self, dvs):
        dvs.setup_db()
        self.setup_db(dvs);

        self.clear_srv_config(dvs)
        vlanID = "100"
        portName = "Ethernet0"
        vlanName = "Vlan" + str(vlanID)
        vrfName = ""
        ipAddress = "10.0.0.0/31"
        srv0MAC = "00:00:00:00:01:11"

        self.create_vlan(vlanID)

        self.create_vlan_member(vlanID, portName)

        # bring up interface
        self.set_admin_status(dvs, portName, "up")
        self.set_admin_status(dvs, vlanName, "up")

        # create vlan interface
        rif_oid = self.create_l3_intf(vlanName, vrfName)

        # assign IP to interface
        self.add_ip_address(vlanName, ipAddress)

        # Set IP address and default route
        cmd = "ip link set eth0 address " + srv0MAC
        dvs.servers[0].runcmd(cmd)
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")
        time.sleep(2)

        # Get neighbor and ARP entry
        dvs.servers[0].runcmd("ping -c 1 10.0.0.0")

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        route = json.loads(intf_entries[0])
        assert route["ip"] == "10.0.0.1"
        assert route["rif"] == rif_oid
        (status, fvs) = tbl.get(intf_entries[0])
        assert status == True

        fvs_dict = dict(fvs)
        assert fvs_dict["SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS"] == srv0MAC

        # Bring link operation state down
        self.set_admin_status(dvs, portName, "down")
        dvs.servers[0].runcmd("ip link set dev eth0 down")
        time.sleep(2)

        #Verify ARP/Neighbor entry is removed
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # Bring link operation state up
        self.set_admin_status(dvs, portName, "up")
        dvs.servers[0].runcmd("ip link set dev eth0 up")

        # Remove IP from interface, and then remove interface
        self.remove_ip_address(vlanName, ipAddress)
        self.remove_l3_intf(vlanName)

        # Remove VLAN member and VLAN
        self.remove_vlan_member(vlanID, portName)
        self.remove_vlan(vlanID)

    def test_arp_flush_on_vlan_member_remove(self, dvs):
        dvs.setup_db()
        self.setup_db(dvs);

        self.clear_srv_config(dvs)
        vlanID = "100"
        portName = "Ethernet0"
        vlanName = "Vlan" + str(vlanID)
        vrfName = ""
        ipAddress = "10.0.0.0/31"
        srv0MAC = "00:00:00:00:01:11"

        self.create_vlan(vlanID)

        self.create_vlan_member(vlanID, portName)

        # bring up interface
        self.set_admin_status(dvs, portName, "up")
        self.set_admin_status(dvs, vlanName, "up")

        # create vlan interface
        rif_oid = self.create_l3_intf(vlanName, vrfName)

        # assign IP to interface
        self.add_ip_address(vlanName, ipAddress)

        # Set IP address and default route
        cmd = "ip link set eth0 address " + srv0MAC
        dvs.servers[0].runcmd(cmd)
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        # Get neighbor and ARP entry
        dvs.servers[0].runcmd("ping -c 1 10.0.0.0")
        time.sleep(2)

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        route = json.loads(intf_entries[0])
        assert route["ip"] == "10.0.0.1"
        assert route["rif"] == rif_oid
        (status, fvs) = tbl.get(intf_entries[0])
        assert status == True

        fvs_dict = dict(fvs)
        assert fvs_dict["SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS"] == srv0MAC
        # Remove port from VLAN
        self.remove_vlan_member(vlanID, portName)

        #Verify ARP/Neighbor entry is removed
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # Remove IP from interface, and then remove interface
        self.remove_ip_address(vlanName, ipAddress)
        self.remove_l3_intf(vlanName)

        # Remove VLAN
        self.remove_vlan(vlanID)

    """
    Below utility functions are required by test_dpb_arp_flush
    TBD: Introduce dvs_neigbor.py function and move these methods to
         that file. Change the code in test_dpb_arp_flush accordingly.
    """
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

    def set_admin_status(self, interface, status):
        tbl = swsscommon.Table(self.cdb, "PORT")
        fvs = swsscommon.FieldValuePairs([("admin_status", status)])
        tbl.set(interface, fvs)
        time.sleep(1)

    def create_l3_intf(self, interface, vrf_name):
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        initial_entries = set(tbl.getKeys())

        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        elif interface.startswith("Loopback"):
            tbl_name = "LOOPBACK_INTERFACE"
        else:
            tbl_name = "INTERFACE"

        tbl = swsscommon.Table(self.cdb, tbl_name)
        if len(vrf_name) == 0:
            fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        else:
            fvs = swsscommon.FieldValuePairs([("vrf_name", vrf_name)])
        tbl.set(interface, fvs)
        time.sleep(1)

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        current_entries = set(tbl.getKeys())
        assert len(current_entries - initial_entries) == 1
        return list(current_entries - initial_entries)[0]

    def remove_l3_intf(self, interface):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        elif interface.startswith("Loopback"):
            tbl_name = "LOOPBACK_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        tbl._del(interface)
        time.sleep(1)

    def add_ip_address(self, interface, ip):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        elif interface.startswith("Loopback"):
            tbl_name = "LOOPBACK_INTERFACE"
        else:
            tbl_name = "INTERFACE"

        tbl = swsscommon.Table(self.cdb, tbl_name)
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(1)

    def remove_ip_address(self, interface, ip):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        elif interface.startswith("Loopback"):
            tbl_name = "LOOPBACK_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        tbl._del(interface + "|" + ip)
        time.sleep(1)

    def clear_srv_config(self, dvs):
        dvs.servers[0].runcmd("ip address flush dev eth0")
        dvs.servers[1].runcmd("ip address flush dev eth0")
        dvs.servers[2].runcmd("ip address flush dev eth0")
        dvs.servers[3].runcmd("ip address flush dev eth0")
        time.sleep(1)

    def create_vlan(self, vlan_id):
        tbl = swsscommon.Table(self.cdb, "VLAN")
        fvs = swsscommon.FieldValuePairs([("vlanid", vlan_id)])
        tbl.set("Vlan" + vlan_id, fvs)
        time.sleep(1)

    def remove_vlan(self, vlan_id):
        tbl = swsscommon.Table(self.cdb, "VLAN")
        tbl._del("Vlan" + vlan_id)
        time.sleep(1)

    def create_vlan_member(self, vlan_id, interface):
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        fvs = swsscommon.FieldValuePairs([("tagging_mode", "untagged")])
        tbl.set("Vlan" + vlan_id + "|" + interface, fvs)
        time.sleep(1)

    def remove_vlan_member(self, vlan_id, interface):
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        tbl._del("Vlan" + vlan_id + "|" + interface)
        time.sleep(1)

    def set_admin_status(self, dvs, interface, status):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN"
        else:
            tbl_name = "PORT"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        fvs = swsscommon.FieldValuePairs([("admin_status", status)])
        tbl.set(interface, fvs)
        time.sleep(1)
