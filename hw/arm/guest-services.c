/*
 * QEMU TCP Tunnelling
 *
 * Copyright (c) 2019 Lev Aronsky <aronsky@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "hw/arm/boot.h"
#include "exec/address-spaces.h"
#include "hw/misc/unimp.h"
#include "sysemu/sysemu.h"
#include "qemu/error-report.h"
#include "hw/platform-bus.h"

#include "hw/arm/n66_iphone6splus.h"
#include "hw/arm/guest-services/general.h"
#include "hw/arm/xnu_trampoline_hook.h"

int32_t guest_svcs_errno = 0;

uint64_t qemu_call_status(CPUARMState *env, const ARMCPRegInfo *ri)
{
    // NOT USED FOR NOW
    return 0;
}

void qemu_call(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    CPUState *cpu = qemu_get_cpu(0);
    qemu_call_t qcall;
    uint64_t i = 0;

    static uint8_t hooks_installed = false;

    if (!value) {
        // Special case: not a regular QEMU call. This is used by our
        // kernel task port patch to notify of the readiness for the
        // hook installation.

        N66MachineState *nms = N66_MACHINE(qdev_get_machine());
        KernelTrHookParams *hook = &nms->hook;

        if (0 != hook->va) {
            //install the hook here because we need the MMU to be already
            //configured and all the memory mapped before installing the hook
            xnu_hook_tr_copy_install(hook->va, hook->pa, hook->buf_va,
                                     hook->buf_pa, hook->code, hook->code_size,
                                     hook->buf_size, hook->scratch_reg);

        }

        if (!hooks_installed) {
            for (i = 0; i < nms->hook_funcs_count; i++) {
                xnu_hook_tr_copy_install(nms->hook_funcs[i].va,
                                         nms->hook_funcs[i].pa,
                                         nms->hook_funcs[i].buf_va,
                                         nms->hook_funcs[i].buf_pa,
                                         nms->hook_funcs[i].code,
                                         nms->hook_funcs[i].code_size,
                                         nms->hook_funcs[i].buf_size,
                                         nms->hook_funcs[i].scratch_reg);
            }
            hooks_installed = true;
        }

        //emulate original opcode: str x20, [x23]
        value = env->xregs[20];
        cpu_memory_rw_debug(cpu, env->xregs[23], (uint8_t*) &value,
                            sizeof(value), 1);

        return;
    }

    // Read the request
    cpu_memory_rw_debug(cpu, value, (uint8_t*) &qcall, sizeof(qcall), 0);

    switch (qcall.call_number) {
        // File Descriptors
        case QC_CLOSE:
            qcall.retval = qc_handle_close(cpu, qcall.args.close.fd);
            break;
        case QC_FCNTL:
            switch (qcall.args.fcntl.cmd) {
                case F_GETFL:
                    qcall.retval = qc_handle_fcntl_getfl(
                        cpu, qcall.args.fcntl.fd);
                    break;
                case F_SETFL:
                    qcall.retval = qc_handle_fcntl_setfl(
                        cpu, qcall.args.fcntl.fd, qcall.args.fcntl.flags);
                    break;
                default:
                    guest_svcs_errno = EINVAL;
                    qcall.retval = -1;
            }
            break;

        // Socket API
        case QC_SOCKET:
            qcall.retval = qc_handle_socket(cpu, qcall.args.socket.domain,
                                            qcall.args.socket.type,
                                            qcall.args.socket.protocol);
            break;
        case QC_ACCEPT:
            qcall.retval = qc_handle_accept(cpu, qcall.args.accept.socket,
                                            qcall.args.accept.addr,
                                            qcall.args.accept.addrlen);
            break;
        case QC_BIND:
            qcall.retval = qc_handle_bind(cpu, qcall.args.bind.socket,
                                          qcall.args.bind.addr,
                                          qcall.args.bind.addrlen);
            break;
        case QC_CONNECT:
            qcall.retval = qc_handle_connect(cpu, qcall.args.connect.socket,
                                             qcall.args.connect.addr,
                                             qcall.args.connect.addrlen);
            break;
        case QC_LISTEN:
            qcall.retval = qc_handle_listen(cpu, qcall.args.listen.socket,
                                            qcall.args.listen.backlog);
            break;
        case QC_RECV:
            qcall.retval = qc_handle_recv(cpu, qcall.args.recv.socket,
                                          qcall.args.recv.buffer,
                                          qcall.args.recv.length,
                                          qcall.args.recv.flags);
            break;
        case QC_SEND:
            qcall.retval = qc_handle_send(cpu, qcall.args.send.socket,
                                          qcall.args.send.buffer,
                                          qcall.args.send.length,
                                          qcall.args.send.flags);
            break;
        case QC_WRITE_FILE:
            qcall.retval = qc_handle_write_file(cpu,
                                       qcall.args.write_file.buffer_guest_ptr,
                                       qcall.args.write_file.length,
                                       qcall.args.write_file.offset,
                                       qcall.args.write_file.index);
            break;
        case QC_READ_FILE:
            qcall.retval = qc_handle_read_file(cpu,
                                       qcall.args.read_file.buffer_guest_ptr,
                                       qcall.args.read_file.length,
                                       qcall.args.read_file.offset,
                                       qcall.args.read_file.index);
            break;
        case QC_SIZE_FILE:
            qcall.retval = qc_handle_size_file(qcall.args.size_file.index);
            break;
        default:
            // TODO: handle unknown call numbers
            break;
    }

    qcall.error = guest_svcs_errno;

    // Write the response
    cpu_memory_rw_debug(cpu, value, (uint8_t*) &qcall, sizeof(qcall), 1);
}
