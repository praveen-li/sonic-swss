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
class TestPortDPBVlan(object):
    def check_syslog(self, dvs, marker, log, expected_cnt):
        (exitcode, num) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep \"%s\" | wc -l" % (marker, log)])
        assert num.strip() >= str(expected_cnt)

    '''
    @pytest.mark.skip()
    '''
    def test_dependency(self, dvs):
        dpb = DPB()
        dvs.setup_db()
        p = Port(dvs, "Ethernet0")
        p.sync_from_config_db()
        dvs.create_vlan("100")
        #print "Created VLAN100"
        dvs.create_vlan_member("100", p.get_name())
        #print "Added Ethernet0 to VLAN100"
        marker = dvs.add_log_marker()
        p.delete_from_config_db()
        #Verify that we are looping on dependency
        time.sleep(2)
        self.check_syslog(dvs, marker, "Cannot remove port as bridge port OID is present", 1)
        assert(p.exists_in_asic_db() == True)

        dvs.remove_vlan_member("100", p.get_name())
        time.sleep(1)
        # Verify that port is deleted
        assert(p.exists_in_asic_db() == False)

        #Create the port back and delete the VLAN
        p.write_to_config_db()
        #print "Added port:%s to config DB"%p.get_name()
        p.verify_config_db()
        #print "Config DB verification passed!"
        p.verify_app_db()
        #print "Application DB verification passed!"
        p.verify_asic_db()
        #print "ASIC DB verification passed!"

        dvs.remove_vlan("100")

    '''
    @pytest.mark.skip()
    '''
    def test_one_port_one_vlan(self, dvs):
        dpb = DPB()
        dvs.setup_db()

        # Breakout testing with VLAN dependency
        dvs.create_vlan("100")
        #print "Created VLAN100"
        dvs.create_vlan_member("100", "Ethernet0")
        #print "Added Ethernet0 to VLAN100"

        p = Port(dvs, "Ethernet0")
        p.sync_from_config_db()
        p.delete_from_config_db()
        assert(p.exists_in_config_db() == False)
        assert(p.exists_in_app_db() == False)
        assert(p.exists_in_asic_db() == True)
        #print "Ethernet0 deleted from config DB and APP DB, waiting to be removed from VLAN"

        dvs.remove_vlan_member("100", "Ethernet0")
        time.sleep(1)
        assert(p.exists_in_asic_db() == False)
        #print "Ethernet0 removed from VLAN and also from ASIC DB"

        dpb.create_child_ports(dvs, p, 4)

        # Breakin testing with VLAN dependency
        port_names = ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"]
        for pname in port_names:
            dvs.create_vlan_member("100", pname)
        #print "Add %s to VLAN"%port_names

        child_ports = []
        for pname in port_names:
            cp = Port(dvs, pname)
            cp.sync_from_config_db()
            cp.delete_from_config_db()
            assert(cp.exists_in_config_db() == False)
            assert(cp.exists_in_app_db() == False)
            assert(cp.exists_in_asic_db() == True)
            child_ports.append(cp)
        #print "Deleted %s from config DB and APP DB"%port_names

        for cp in child_ports:
            dvs.remove_vlan_member("100", cp.get_name())
            time.sleep(1)
            assert(cp.exists_in_asic_db() == False)
        #print "Deleted %s from VLAN"%port_names

        p.write_to_config_db()
        #print "Added port:%s to config DB"%p.get_name()
        p.verify_config_db()
        #print "Config DB verification passed!"
        p.verify_app_db()
        #print "Application DB verification passed!"
        p.verify_asic_db()
        #print "ASIC DB verification passed!"

        dvs.remove_vlan("100")

    '''
    @pytest.mark.skip()
    '''
    def test_one_port_multiple_vlan(self, dvs):
        dpb = DPB()
        dvs.setup_db()

        dvs.create_vlan("100")
        dvs.create_vlan("101")
        dvs.create_vlan("102")
        #print "Created VLAN100, VLAN101, and VLAN102"
        dvs.create_vlan_member("100", "Ethernet0")
        dvs.create_vlan_member("101", "Ethernet0")
        dvs.create_vlan_member("102", "Ethernet0")
        #print "Added Ethernet0 to all three VLANs"

        p = Port(dvs, "Ethernet0")
        p.sync_from_config_db()
        p.delete_from_config_db()
        assert(p.exists_in_config_db() == False)
        assert(p.exists_in_app_db() == False)
        assert(p.exists_in_asic_db() == True)
        #print "Ethernet0 deleted from config DB and APP DB, waiting to be removed from VLANs"

        dvs.remove_vlan_member("100", "Ethernet0")
        time.sleep(1)
        assert(p.exists_in_asic_db() == True)
        #print "Ethernet0 removed from VLAN100 and its still present in ASIC DB"

        dvs.remove_vlan_member("101", "Ethernet0")
        time.sleep(1)
        assert(p.exists_in_asic_db() == True)
        #print "Ethernet0 removed from VLAN101 and its still present in ASIC DB"

        dvs.remove_vlan_member("102", "Ethernet0")
        time.sleep(1)
        assert(p.exists_in_asic_db() == False)
        #print "Ethernet0 removed from VLAN101 and also from ASIC DB"

        dpb.create_child_ports(dvs, p, 4)
        #print "1X40G ---> 4x10G verified"

        # Breakin
        port_names = ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"]
        for pname in port_names:
            cp = Port(dvs, pname)
            cp.sync_from_config_db()
            cp.delete_from_config_db()
            assert(cp.exists_in_config_db() == False)
            assert(cp.exists_in_app_db() == False)
            assert(cp.exists_in_asic_db() == False)
        #print "Deleted %s and verified all DBs"%port_names

        #Add back Ethernet0
        p.write_to_config_db()
        p.verify_config_db()
        p.verify_app_db()
        p.verify_asic_db()
        #print "Added port:%s and verified all DBs"%p.get_name()

        # Remove all three VLANs
        dvs.remove_vlan("100")
        dvs.remove_vlan("101")
        dvs.remove_vlan("102")
        #print "All three VLANs removed"

    '''
    @pytest.mark.skip()
    '''
    def test_all_port_10_vlans(self, dvs):
        num_vlans = 10
        start_vlan = 100
        num_ports = 32
        port_names = []
        vlan_names = []

        dvs.setup_db()
        for i in range(num_ports):
            port_names.append("Ethernet" + str(i*4))

        for i in range(num_vlans):
            vlan_names.append(str(start_vlan + i))

        for vlan_name in vlan_names:
            dvs.create_vlan(vlan_name)
        #print "%d VLANs created"%num_vlans

        for port_name in port_names:
            for vlan_name in vlan_names:
                dvs.create_vlan_member_tagged(vlan_name, port_name)
        #print "All %d ports are added to all %d VLANs"%(num_ports,num_vlans)

        ports = []
        for port_name in port_names:
            p = Port(dvs, port_name)
            ports.append(p)
            p.sync_from_config_db()
            p.delete_from_config_db()
            #print "Deleted %s from config DB"%port_name
            assert(p.exists_in_config_db() == False)
            assert(p.exists_in_app_db() == False)
            assert(p.exists_in_asic_db() == True)
            for vlan_name in vlan_names:
                dvs.remove_vlan_member(vlan_name, port_name)
            time.sleep(1)
            assert(p.exists_in_asic_db() == False)
        #print "All %d ports are removed from all %d VLANs and deleted"%(num_ports,num_vlans)

        for p in ports:
            p.write_to_config_db()
            p.verify_config_db()
            p.verify_app_db()
            p.verify_asic_db()
        #print "Re-created all %d ports"%num_ports

        for vlan_name in vlan_names:
            dvs.remove_vlan(vlan_name)
        #print "All %d VLANs removed"%num_vlans
