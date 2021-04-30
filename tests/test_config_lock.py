import time
import json
import pexpect


from redis import Redis
from threading import Thread

RECV_SIZE=128

class TestConfigLock:
    """
        These tests may run 2 or more thread to excercise ConfigDbLock class in
        config/main.py and makes sure all below possible paths are exercised.
        acquireLock():
            -- AcquireLock.
            -- Can not acquire lock, Reset Timer & Abort.
            -- Can not acquire lock, Abort.
        reacquireLock():
            -- Lock Reacquired.
            -- Lock Timer Extended
            -- some other process is holding the lock.
        _releaseLock():
            -- current process holding the lock then release it.
    """

    def set_up(self, dvs):
        self.lock_expire_timer = 10
        self.dvs = dvs
        self.cdb = self.dvs.get_config_db()
        self.expCount = 0
        return

    def check_lock_with_wait(self, wait=10):
        """
            Check LOCK Table and TTL Value from config DB, if exists, either
            wait it to expire or fail
            Parameters:
                wait(int): wait period
            Returns:
                void
        """
        # if lock already exists
        t = self.get_config_db_lock("Check Lock")
        if len(t) != 0 and wait != 0:
            print("wait for: {}secs".format(wait))
            time.sleep(wait)
        elif len(t) != 0:
            assert False
        return

    def get_config_db_lock(self, tcName):
        """
            Get LOCK Table and TTL Value from config DB
            Parameters:
                void
            Returns:
                table, ttl (dict, int): LOCK Table, expire timer
        """
        table = self.cdb.get_entry('LOCK', 'configDbLock')
        print("{}: {}".format(tcName, table))
        return table

    def test_lock_2nd_config_fail(self, dvs):
        """
            Acquire a lock with command config which stops on prompt.
            Run another config command in between to make sure lock is not acquired.
            Then say yes\No on the prompt of first command.

            Expectation: Second command should not acquire lock, first command should re-extend the timer.

            Example Run:

            Terminal 1:
            admin@lnos-x1-a-fab01:~$ sudo config save
            Existing file will be overwritten, continue? [y/N]:

            Terminal 2:
            admin@lnos-x1-a-fab01:~$ sudo config save
            :::Can not acuire lock, Abort:::

            Terminal 1:
            admin@lnos-x1-a-fab01:~$ sudo config save
            Existing file will be overwritten, continue? [y/N]:N <<< from here
        """

        def config_save_th_1(tcName):
            try:
                exitCode, child = self.dvs.runcmd_interactive('config save')
                # -- AcquireLock.
                child.settimeout(3)
                out = str(child.recv(RECV_SIZE))
                print("{}: out:{}".format(tcName, out))
                assert 'continue' in out
                # sleep let other thread run
                time.sleep(4)
                # -- send No
                child.send("N"+"\n")
                time.sleep(2)
                out = str(child.recv(RECV_SIZE))
                print("{}: out:{}".format(tcName, out))
                assert 'Aborted' in out
                child.close()
                # -- Make Sure no lock in CONFIG_DB
                t = self.get_config_db_lock(tcName)
                assert len(t) == 0
            except Exception as e:
                print(e)
                self.expCount = self.expCount + 1
            return

        def config_save_th_2(tcName):
            try:
                time.sleep(2)
                exitCode, child = self.dvs.runcmd_interactive('config save')
                # -- Can not acquire lock, Abort.
                child.settimeout(3)
                out = str(child.recv(RECV_SIZE))
                print("{}: out:{}".format(tcName, out))
                assert 'Can not acquire lock, Abort' in out
                child.close()
            except Exception as e:
                print(e)
                self.expCount = self.expCount + 1
            return

        # Main Func Code
        self.set_up(dvs)
        self.check_lock_with_wait();
        t1 = Thread(target=config_save_th_1, args=("test_lock_2nd_config_fail TH1",))
        t2 = Thread(target=config_save_th_2, args=("test_lock_2nd_config_fail TH2",))
        t1.start()
        t2.start()
        t1.join()
        t2.join()
        assert self.expCount == 0

        return

    def test_lock_2nd_config_succ(self, dvs):
        """
            run first config command which stops on prompt.
            let the lock timer expires (wait for 10s),
            run second config command in Terminal 2, which stops on prompt too.
            then within 10 secs say yes on the prompt of first command.

            Expectation: Second command should acquire lock, first command should not be able to acquire lock after prompt.

            Example Run:
            Terminal 1:
            admin@lnos-x1-a-fab01:~$ sudo config save
            Existing file will be overwritten, continue? [y/N]:  <<< wait for 10 secs

            Terminal 2:
            admin@lnos-x1-a-fab01:~$ sudo config load
            Load config from the file /etc/sonic/config_db.json? [y/N]:  <<< Switch to Terminal 1

            Terminal 1:
            Existing file will be overwritten, continue? [y/N]:  <<< start from here, Say yes
            :::Can not acquire lock, Abort:::
        """

        def config_load_th_1(tcName):
            try:
                exitCode, child = self.dvs.runcmd_interactive('config load')
                # -- AcquireLock.
                child.settimeout(3)
                out = str(child.recv(RECV_SIZE))
                print("{}: out:{}".format(tcName, out))
                assert 'Load config' in out
                # sleep let LOCK expire and other thread will acquire lock
                time.sleep(self.lock_expire_timer+3)
                child.send("y\n")
                time.sleep(3)
                out = str(child.recv(RECV_SIZE))
                print("{}: out:{}".format(tcName, out))
                assert 'Can not acquire lock' in out
                child.close()
            except Exception as e:
                print(e)
                self.expCount = self.expCount + 1
            return

        def config_save_th_2(tcName):
            try:
                exitCode, child = self.dvs.runcmd_interactive('config save')
                # -- AcquireLock.
                child.settimeout(3)
                out = str(child.recv(RECV_SIZE))
                print("{}: out:{}".format(tcName, out))
                assert 'continue' in out
                # sleep let other thread run
                time.sleep(4)
                # say yes to save config and exercise lock extended timer
                child.send("y"+"\n")
                time.sleep(3)
                out = str(child.recv(RECV_SIZE))
                print("{}: out:{}".format(tcName, out))
                assert 'sonic-cfggen -d --print' in out
                child.close()
                # -- exercise: current process holding the lock then release it.
                t = self.get_config_db_lock(tcName)
                assert len(t) == 0
            except Exception as e:
                print(e)
                self.expCount = self.expCount + 1
            return

        # Main Func Code
        self.set_up(dvs)
        self.check_lock_with_wait();
        t1 = Thread(target=config_load_th_1, args=("test_lock_2nd_config_succ TH1",))
        t2 = Thread(target=config_save_th_2, args=("test_lock_2nd_config_succ TH2",))
        t1.start()
        time.sleep(self.lock_expire_timer+1)
        t2.start()
        t1.join()
        t2.join()
        assert self.expCount == 0

        return

    def test_lock_config_reacquire(self, dvs):
        """
            run first config command which stops on prompt.
            let the lock timer expires (wait for 10s),
            LOCK in config DB must not be present after step 2
            then say yes on the prompt of first command to reacquire the lock.

            Expectation: LOCK in config DB must not be present after step 2. LOCK timer must be reacquired in step3.

            Example Run:
            Terminal 1:
            admin@lnos-x1-a-fab01:~$ sudo config save
            Existing file will be overwritten, continue? [y/N]:  <<< wait for 10 secs

            root@d3c0e1fe77c6:/praveen# redis-cli -n 4 hgetall "LOCK|configDbLock"
            (empty list or set)

            Existing file will be overwritten, continue? [y/N]:y  <<< start from here, Say yes

            Check DB for LOCK and timer if done by automation. Or Parse /var/log/syslog
            root@d3c0e1fe77c6:/praveen# tail -25 /var/log/syslog | grep "Lock Reacquired"
            Aug  8 00:10:43.667209 d3c0e1fe77c6 DEBUG #config: :::Lock Reacquired:::
        """
        # Main Func Code
        self.set_up(dvs)
        self.check_lock_with_wait();
        tcName = "test_lock_config_reacquire"
        # start the command in interactive mode
        exitCode, child = self.dvs.runcmd_interactive('config save')
        # -- AcquireLock.
        child.settimeout(3)
        out = str(child.recv(RECV_SIZE))
        print("{}: out:{}".format(tcName, out))
        assert 'continue' in out
        # sleep to release the lock
        time.sleep(self.lock_expire_timer+1)
        t = self.get_config_db_lock(tcName)
        assert len(t) == 0
        # say yes to save config and exercise lock reacquired
        child.send("y\n")
        time.sleep(3)
        out = str(child.recv(RECV_SIZE))
        print("{}: out:{}".format(tcName, out))
        assert 'sonic-cfggen -d --print' in out
        child.close()
        # check no LOCK in config DB
        t = self.get_config_db_lock(tcName)
        assert len(t) == 0

        return

    def test_config_donot_save_lock(self, dvs):
        """
            Run config save command and
            make sure config file does not have LOCK table.

            Example Run:
            admin@lnos-x1-a-fab01:~$ sudo config save -y
            to test manually
            >> $cat  /etc/sonic/config_db.json | grep LOCK
            In automation:
            load that file --> config = json.loads(readJsonFile(configFile))
            configFile = /etc/sonic/config_db.json
            makes sure config.get('LOCK') == null
        """
        # Main Func Code
        self.set_up(dvs)
        self.check_lock_with_wait();
        tcName = "test_config_donot_save_lock"
        # start the command in interactive mode
        exitCode, child = self.dvs.runcmd_interactive('config save')
        # -- AcquireLock.
        child.settimeout(3)
        out = str(child.recv(RECV_SIZE))
        print("{}: out:{}".format(tcName, out))
        assert 'continue' in out
        # check lock in Config DB
        t = self.get_config_db_lock(tcName)
        assert len(t) != 0
        # say yes to save config and exercise lock extension
        child.send("y\n")
        time.sleep(3)
        out = str(child.recv(RECV_SIZE))
        print("{}: out:{}".format(tcName, out))
        assert 'sonic-cfggen -d --print' in out
        child.close()
        # Check: no LOCK in saved config
        exitCode, out = self.dvs.runcmd("grep LOCK /etc/sonic/config_db.json")
        print("cmd out: {}".format(out))
        assert "LOCK" not in out

        return

    def test_lock_reset_timer(self, dvs):
        """
            Put lock in config DB with no timer on it.
            Wait for 10s, LOCK must not expire.[Not neccessary]
            Run config command, it must reset timer not lock and abort.
            Wait for 10s, LOCK should expire.
            Run config command, it must go ahead i.e. should not abort.

            This is important case, if a LOCK without timer exist in config DB
            due to a bug or wrong config load.

            Example Run:
            root@d3c0e1fe77c6:/praveen# redis-cli -n 4 hset "LOCK|configDbLock" PID 12345
            (integer) 1
            root@d3c0e1fe77c6:/praveen# redis-cli -n 4 hgetall "LOCK|configDbLock"
            1) "PID"
            2) "12345"

            root@d3c0e1fe77c6:/praveen# config save
            :::Can not acquire lock, Reset Timer & Abort:::

            root@d3c0e1fe77c6:/praveen# redis-cli -n 4 hgetall "LOCK|configDbLock"
            (empty list or set)
            root@d3c0e1fe77c6:/praveen# config save
            Existing file will be overwritten, continue? [y/N]:
        """
        # Main Func Code
        self.set_up(dvs)
        self.check_lock_with_wait();
        tcName = "test_lock_reset_timer"

        # add permanent entry for ock in Config DB
        table = self.cdb.create_entry('LOCK', 'configDbLock', {'PID': '5555'})
        # start the command in interactive mode, it should Reset Timer
        exitCode, child = self.dvs.runcmd_interactive('config save')
        child.settimeout(3)
        out = str(child.recv(RECV_SIZE))
        print("{}: out:{}".format(tcName, out))
        assert 'Reset Timer' in out
        child.close()
        # let time expire
        time.sleep(self.lock_expire_timer+1)
        # check lock in Config DB
        t = self.get_config_db_lock(tcName)
        assert len(t) == 0
        # start the command in interactive mode, it should Run successfully
        exitCode, child = self.dvs.runcmd_interactive('config save -y')
        child.settimeout(3)
        time.sleep(3)
        out = str(child.recv(RECV_SIZE))
        print("{}: out:{}".format(tcName, out))
        assert 'sonic-cfggen -d --print' in out
        child.close()

        return
