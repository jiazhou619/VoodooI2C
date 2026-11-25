//
//  VoodooI2CACPIController.cpp
//  VoodooI2C
//
//  Created by Alexandre on 02/08/2017.
//  Copyright © 2017 Alexandre Daoud. All rights reserved.
//

#include "VoodooI2CACPIController.hpp"
#include "VoodooI2CControllerConstants.hpp"

#define super VoodooI2CController
OSDefineMetaClassAndStructors(VoodooI2CACPIController, VoodooI2CController);

#define LPSS_PRIVATE_CLOCK_GATING       0x800

void VoodooI2CACPIController::setClkGating(VoodooI2CState enabled) {
    if (!legacy_intel_lpss)
        return;

    writeRegister(enabled, LPSS_PRIVATE_CLOCK_GATING);
}

IOReturn VoodooI2CACPIController::setACPIPowerState(VoodooI2CState enabled) {
    if (enabled) {
        IOLog("%s::%s Set ACPI power state _PS0\n", getName(), physical_device.name);
        if (!physical_device.acpi_device->evaluateObject("_PS0"))
            return kIOReturnNoPower;
    } else {
        IOLog("%s::%s Set ACPI power state _PS3\n", getName(), physical_device.name);
        if (!physical_device.acpi_device->evaluateObject("_PS3"))
            return kIOReturnNoPower;
    }
    return kIOReturnSuccess;
}

IOReturn VoodooI2CACPIController::setPowerState(unsigned long whichState, IOService * whatDevice) {
    if (whatDevice != this)
        return kIOPMAckImplied;

    if (whichState == 0) {  // index of kIOPMPowerOff state in VoodooI2CIOPMPowerStates
        if (physical_device.awake) {
            physical_device.awake = false;
            setClkGating(kVoodooI2CStateOff);
            unmapMemory();
            setACPIPowerState(kVoodooI2CStateOff);
            IOLog("%s::%s Going to sleep\n", getName(), physical_device.name);
        }
    } else {
        if (!physical_device.awake) {
            setACPIPowerState(kVoodooI2CStateOn);
            if (mapMemory() != kIOReturnSuccess)
                IOLog("%s::%s Could not map memory\n", getName(), physical_device.name);
            setClkGating(kVoodooI2CStateOn);
            physical_device.awake = true;
            IOLog("%s::%s Woke up\n", getName(), physical_device.name);
        }
    }
    return kIOPMAckImplied;
}

bool VoodooI2CACPIController::start(IOService* provider) {
    if (!super::start(provider)) {
        return false;
    }

    legacy_intel_lpss = getProperty(kI2CPropIntelLpssKey) != nullptr;
    physical_device.acpi_device = OSDynamicCast(IOACPIPlatformDevice, provider);
    setACPIPowerState(kVoodooI2CStateOn);

    /*
     * AMD Cezanne (AMDI0010) shares its I2C bus with the PSP.  On a warm
     * reboot the DesignWare state machine and the PSP's bus-arbitration state
     * survive from the previous session; Linux only gained a workaround for
     * this in 2026 (ARBITRATION_SEMAPHORE / lost-arbitration on touchpads).
     * On this machine only a full power cut reliably clears it.
     *
     * The firmware exposes an ACPI RSET method (SRAD -> SMU register write)
     * that resets the controller.  VoodooI2C never evaluated it.  Invoke it
     * once here, before any registers are configured or nubs published, to
     * reproduce the cold-power reset in software.
     *
     * Restrict to _UID==1 (I2CB, the Synaptics touchpad controller).  I2CC
     * drives the touchscreen and I2CD the CS35L41 amplifier; resetting those
     * would disturb devices the user reports as working.
     */
    UInt32 controller_uid = 0;
    bool is_touchpad_bus =
        physical_device.acpi_device &&
        physical_device.acpi_device->evaluateInteger("_UID", &controller_uid) == kIOReturnSuccess &&
        controller_uid == 1;
    setProperty("Controller UID", controller_uid, 32);
    setProperty("Is Touchpad Bus", is_touchpad_bus);

    if (is_touchpad_bus && !legacy_intel_lpss) {
        OSObject* rset_result = nullptr;
        IOReturn rset_status = physical_device.acpi_device->evaluateObject("RSET", &rset_result);
        if (rset_result) {
            rset_result->release();
            rset_result = nullptr;
        }
        setProperty("Controller RSET Status", rset_status, 32);
        IOLog("%s::%s ACPI RSET status 0x%x\n", getName(), physical_device.name, rset_status);
        if (rset_status == kIOReturnSuccess) {
            setProperty("Controller RSET Done", true);
            IODelay(100);  // let the SMU finish the reset
        } else {
            setProperty("Controller RSET Done", false);
        }
    }

    if (mapMemory() != kIOReturnSuccess) {
        IOLog("%s::%s Could not map memory\n", getName(), physical_device.name);
        return false;
    }

    setClkGating(kVoodooI2CStateOn);

    if (publishNub() != kIOReturnSuccess) {
        IOLog("%s::%s Could not publish nub\n", getName(), physical_device.name);
        return false;
    }

    registerService();

    return true;
}

void VoodooI2CACPIController::stop(IOService* provider) {
    setClkGating(kVoodooI2CStateOff);
    setACPIPowerState(kVoodooI2CStateOff);

    super::stop(provider);
}
