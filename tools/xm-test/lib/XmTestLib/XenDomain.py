#!/usr/bin/python
"""
 Copyright (C) International Business Machines Corp., 2005
 Author: Dan Smith <danms@us.ibm.com>

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; under version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

"""

import sys
import commands
import re
import time

from Xm import *
from arch import *
from Test import *
from config import *
from Console import *
from XenDevice import *
from DomainTracking import *
from acm import *


DOM0_UUID = "00000000-0000-0000-0000-000000000000"


def getDefaultKernel():
    return arch.getDefaultKernel()

def getRdPath():
    return arch.getRdPath()

def getUniqueName():
    """Get a uniqueish name for use in a domain"""
    unixtime = int(time.time())
    test_name = sys.argv[0]
    test_name = re.sub("\.test", "", test_name)
    test_name = re.sub("[\/\.]", "", test_name)
    name = "%s-%i" % (test_name, unixtime)

    return name

class XenConfig:
    """An object to help create a xen-compliant config file"""
    def __init__(self):
        self.defaultOpts = {}

        # These options need to be lists
        self.defaultOpts["disk"] = []
        self.defaultOpts["vif"]  = []
        self.defaultOpts["vtpm"] = []
        if isACMEnabled():
            #A default so every VM can start with ACM enabled
            self.defaultOpts["access_control"] = ['policy=xm-test,label=red']

        self.opts = self.defaultOpts

    def toString(self):
        """Convert this config to a string for writing out
        to a file"""
        string = "# Xen configuration generated by xm-test\n"
        for k, v in self.opts.items():
            if isinstance(v, int):
                piece = "%s = %i" % (k, v)
            elif isinstance(v, list) and v:
                piece = "%s = %s" % (k, v)
            elif isinstance(v, str) and v:
                piece = "%s = \"%s\"" % (k, v)
            else:
                piece = None

            if piece:
                string += "%s\n" % piece

        return string

    def write(self, filename):
        """Write this config out to filename"""
        output = file(filename, "w")
        output.write(self.toString())
        output.close()
        ACMPrepareSystem(self.opts)

    def __str__(self):
        """When used as a string, we represent ourself by a config
        filename, which points to a temporary config that we write
        out ahead of time"""
        filename = "/tmp/xm-test.conf"
        self.write(filename)
        return filename

    def setOpt(self, name, value):
        """Set an option in the config"""
        if name in self.opts.keys() and isinstance(self.opts[name] ,
                                        list) and not isinstance(value, list):
                self.opts[name] = [value]
        # "extra" is special so append to it.
        elif name == "extra" and name in self.opts.keys():
            self.opts[name] += " %s" % (value)
        else:
            self.opts[name] = value

    def appOpt(self, name, value):
        """Append a value to a list option"""
        if name in self.opts.keys() and isinstance(self.opts[name], list):
            self.opts[name].append(value)

    def getOpt(self, name):
        """Return the value of a config option"""
        if name in self.opts.keys():
            return self.opts[name]
        else:
            return None

    def setOpts(self, opts):
        """Batch-set options from a dictionary"""
        for k, v in opts.items():
            self.setOpt(k, v)

    def clearOpts(self, name=None):
        """Clear one or all config options"""
        if name:
            self.opts[name] = self.defaultOpts[name]
        else:
            self.opts = self.defaultOpts

class DomainError(Exception):
    def __init__(self, msg, extra="", errorcode=0):
        self.msg = msg
        self.extra = extra
        try:
            self.errorcode = int(errorcode)
        except Exception, e:
            self.errorcode = -1

    def __str__(self):
        return str(self.msg)


class XenDomain:

    def __init__(self, name=None, config=None, isManaged=False):
        """Create a domain object.
        @param config: String filename of config file
        """

        if name:
            self.name = name
        else:
            self.name = getUniqueName()

        self.config = config
        self.console = None
        self.devices = {}
        self.netEnv = "bridge"

        if os.getenv("XM_MANAGED_DOMAINS"):
            isManaged = True
        self.isManaged = isManaged

        # Set domain type, either PV for ParaVirt domU or HVM for
        # FullVirt domain
        if ENABLE_HVM_SUPPORT:
            self.type = "HVM"
        else:
            self.type = "PV"

    def start(self, noConsole=False):

        if not self.isManaged:
            ret, output = traceCommand("xm create %s" % self.config)
        else:
            ret, output = traceCommand("xm new %s" % self.config)
            if ret != 0:
                _ret, output = traceCommand("xm delete " +
                                            self.config.getOpt("name"))
            else:
                ret, output = traceCommand("xm start " +
                                           self.config.getOpt("name"))
                addManagedDomain(self.config.getOpt("name"))

        if ret != 0:
            raise DomainError("Failed to create domain",
                              extra=output,
                              errorcode=ret)

        # HVM domains require waiting for boot
        if self.getDomainType() == "HVM":
            waitForBoot()

        # Go through device list and run console cmds
        for dev in self.devices.keys():
            self.devices[dev].execAddCmds()

        if self.console and noConsole == True:
            self.closeConsole()

        elif self.console and noConsole == False:
            return self.console

        elif not self.console and noConsole == False:
            return self.getConsole()

    def stop(self):
        prog = "xm"
        cmd = " shutdown "

        self.removeAllDevices()

        if self.console:
            self.closeConsole()

        ret, output = traceCommand(prog + cmd + self.config.getOpt("name"))

        return ret

    def destroy(self):
        prog = "xm"
        cmd = " destroy "

        self.removeAllDevices()

        if self.console:
            self.closeConsole()

        ret, output = traceCommand(prog + cmd + self.config.getOpt("name"))
        if self.isManaged:
            ret, output = traceCommand(prog + " delete " +
                                       self.config.getOpt("name"))
            delManagedDomain(self.config.getOpt("name"))

        return ret

    def getName(self):
        return self.name

    def getId(self):
        return domid(self.getName());

    def getDomainType(self):
        return self.type

    def closeConsole(self):
        # The domain closeConsole command must be called by tests, not the
        # console's close command. Once close is called, the console is
        # gone. You can't get history or anything else from it.
        if self.console:
            self.console._XmConsole__closeConsole()
            self.console = None

    def getConsole(self):
        if self.console:
            self.closeConsole()

        self.console = XmConsole(self.getName())
        # Activate the console
        self.console.sendInput("input")

        return self.console

    def newDevice(self, Device, *args):
        """Device Factory: Generic factory for creating new XenDevices.
           All device creation should be done through the XenDomain
           factory. Supply a XenDevice instance and its args and the
           constructor will be called."""
        # Make sure device with id hasn't already been added
        if self.devices.has_key(args[0]):
            raise DeviceError("Error: Domain already has device %s" % args[0])

        # Call constructor for supplied Device instance
        dargs = (self,)
        dargs += args
        dev = apply(Device, dargs)

        if self.isRunning():
            # Note: This needs to be done, XenDevice should have an attach
            #       method.
            print "Domain is running, need to attach new device to domain."

        self.devices[dev.id] = dev
        self.config.appOpt(dev.configNode, str(dev))
        return dev

    def removeDevice(self, id):
        if self.devices.has_key(id):
            self.devices[id].removeDevice()

    def removeAllDevices(self):
        for k in self.devices.keys():
            self.removeDevice(k)

    def isRunning(self):
        return isDomainRunning(self.name)

    def getNetEnv(self):
        # We need to know the network environment: bridge, NAT, or routed.
        return self.netEnv

    def getDevice(self, id):
        dev = self.devices[id]
        if dev:
            return dev
        print "Device %s not found for domain %s" % (id, self.getName())


class XmTestDomain(XenDomain):

    def __init__(self, name=None, extraConfig=None,
                 baseConfig=arch.configDefaults, isManaged=False):
        """Create a new xm-test domain
        @param name: The requested domain name
        @param extraConfig: Additional configuration options
        @param baseConfig: The initial configuration defaults to use
        """
        config = XenConfig()
        config.setOpts(baseConfig)
        if extraConfig:
            config.setOpts(extraConfig)

        if name:
            config.setOpt("name", name)
        elif not config.getOpt("name"):
            config.setOpt("name", getUniqueName())

        XenDomain.__init__(self, config.getOpt("name"), config=config,
                           isManaged=isManaged)

    def minSafeMem(self):
        return arch.minSafeMem

class XmTestNetDomain(XmTestDomain):

    def __init__(self, name=None, extraConfig=None,
                 baseConfig=arch.configDefaults):
        """Create a new xm-test domain with one network device
        @param name: The requested domain name
        @param extraConfig: Additional configuration options
        @param baseConfig: The initial configuration defaults to use
        """
        config = XenConfig()
        config.setOpts(baseConfig)
        if extraConfig:
            config.setOpts(extraConfig)

        if name:
            config.setOpt("name", name)
        elif not config.getOpt("name"):
            config.setOpt("name", getUniqueName())

        XenDomain.__init__(self, config.getOpt("name"), config=config)

        # Add one network devices to domain
        self.newDevice(XenNetDevice, "eth0")


if __name__ == "__main__":

    c = XenConfig()

    c.setOpt("foo", "bar")
    c.setOpt("foob", 1)
    opts = {"opt1" : 19,
            "opt2" : "blah"}
    c.setOpts(opts)

    c.setOpt("disk", "phy:/dev/ram0,hda1,w")
    c.appOpt("disk", "phy:/dev/ram1,hdb1,w")

    print str(c)



#    c.write("/tmp/foo.conf")

#    d = XmTestDomain();
#
#    d.start();

