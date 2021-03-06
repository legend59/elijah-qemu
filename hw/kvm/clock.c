/*
 * QEMU KVM support, paravirtual clock device
 *
 * Copyright (C) 2011 Siemens AG
 *
 * Authors:
 *  Jan Kiszka        <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL version 2.
 * See the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu-common.h"
#include "sysemu.h"
#include "kvm.h"
#include "hw/sysbus.h"
#include "hw/kvm/clock.h"

#include <linux/kvm.h>
#include <linux/kvm_para.h>

typedef struct KVMClockState {
    SysBusDevice busdev;
    uint64_t clock;
    bool clock_valid;
} KVMClockState;

static void kvmclock_pre_save(void *opaque)
{
    KVMClockState *s = opaque;
    struct kvm_clock_data data;
    int ret;

    if (s->clock_valid) {
        return;
    }
    ret = kvm_vm_ioctl(kvm_state, KVM_GET_CLOCK, &data);
    if (ret < 0) {
        fprintf(stderr, "KVM_GET_CLOCK failed: %s\n", strerror(ret));
        data.clock = 0;
    }
    s->clock = data.clock;
    /*
     * If the VM is stopped, declare the clock state valid to avoid re-reading
     * it on next vmsave (which would return a different value). Will be reset
     * when the VM is continued.
     */
    s->clock_valid = !runstate_is_running();
}

static int kvmclock_post_load(void *opaque, int version_id)
{
    KVMClockState *s = opaque;
    struct kvm_clock_data data;

    data.clock = s->clock;
    data.flags = 0;
    return kvm_vm_ioctl(kvm_state, KVM_SET_CLOCK, &data);
}

static void kvmclock_vm_state_change(void *opaque, int running,
                                     RunState state)
{
    KVMClockState *s = opaque;
    CPUArchState *penv = first_cpu;
    int cap_clock_ctrl = kvm_check_extension(kvm_state, KVM_CAP_KVMCLOCK_CTRL);
    int ret;

    if (running) {
        s->clock_valid = false;

        if (!cap_clock_ctrl) {
            return;
        }
        for (penv = first_cpu; penv != NULL; penv = penv->next_cpu) {
            ret = kvm_vcpu_ioctl(penv, KVM_KVMCLOCK_CTRL, 0);
            if (ret) {
                if (ret != -EINVAL) {
                    fprintf(stderr, "%s: %s\n", __func__, strerror(-ret));
                }
                return;
            }
        }
    }
}

static int kvmclock_init(SysBusDevice *dev)
{
    KVMClockState *s = FROM_SYSBUS(KVMClockState, dev);

    qemu_add_vm_change_state_handler(kvmclock_vm_state_change, s);
    return 0;
}

static const VMStateDescription kvmclock_vmsd = {
    .name = "kvmclock",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .pre_save = kvmclock_pre_save,
    .post_load = kvmclock_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT64(clock, KVMClockState),
        VMSTATE_END_OF_LIST()
    }
};

static void kvmclock_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = kvmclock_init;
    dc->no_user = 1;
    dc->vmsd = &kvmclock_vmsd;
}

static TypeInfo kvmclock_info = {
    .name          = "kvmclock",
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(KVMClockState),
    .class_init    = kvmclock_class_init,
};

/* Note: Must be called after VCPU initialization. */
void kvmclock_create(void)
{
    if (kvm_enabled() &&
        first_cpu->cpuid_kvm_features & ((1ULL << KVM_FEATURE_CLOCKSOURCE) |
                                         (1ULL << KVM_FEATURE_CLOCKSOURCE2))) {
        sysbus_create_simple("kvmclock", -1, NULL);
    }
}

static void kvmclock_register_types(void)
{
    type_register_static(&kvmclock_info);
}

type_init(kvmclock_register_types)
