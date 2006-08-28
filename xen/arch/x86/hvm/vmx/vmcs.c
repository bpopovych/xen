/*
 * vmcs.c: VMCS management
 * Copyright (c) 2004, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 */

#include <xen/config.h>
#include <xen/init.h>
#include <xen/mm.h>
#include <xen/lib.h>
#include <xen/errno.h>
#include <xen/domain_page.h>
#include <asm/current.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>
#include <asm/msr.h>
#include <asm/hvm/hvm.h>
#include <asm/hvm/io.h>
#include <asm/hvm/support.h>
#include <asm/hvm/vmx/vmx.h>
#include <asm/hvm/vmx/vmcs.h>
#include <asm/flushtlb.h>
#include <xen/event.h>
#include <xen/kernel.h>
#include <xen/keyhandler.h>
#include <asm/shadow.h>

static int vmcs_size;
static int vmcs_order;
static u32 vmcs_revision_id;

void vmx_init_vmcs_config(void)
{
    u32 vmx_msr_low, vmx_msr_high;

    if ( vmcs_size )
        return;

    rdmsr(MSR_IA32_VMX_BASIC_MSR, vmx_msr_low, vmx_msr_high);

    vmcs_revision_id = vmx_msr_low;

    vmcs_size  = vmx_msr_high & 0x1fff;
    vmcs_order = get_order_from_bytes(vmcs_size);
}

static struct vmcs_struct *vmx_alloc_vmcs(void)
{
    struct vmcs_struct *vmcs;

    if ( (vmcs = alloc_xenheap_pages(vmcs_order)) == NULL )
    {
        DPRINTK("Failed to allocate VMCS.\n");
        return NULL;
    }

    memset(vmcs, 0, vmcs_size); /* don't remove this */
    vmcs->vmcs_revision_id = vmcs_revision_id;

    return vmcs;
}

static void vmx_free_vmcs(struct vmcs_struct *vmcs)
{
    free_xenheap_pages(vmcs, vmcs_order);
}

static void __vmx_clear_vmcs(void *info)
{
    struct vcpu *v = info;

    __vmpclear(virt_to_maddr(v->arch.hvm_vmx.vmcs));

    v->arch.hvm_vmx.active_cpu = -1;
    v->arch.hvm_vmx.launched   = 0;
}

static void vmx_clear_vmcs(struct vcpu *v)
{
    int cpu = v->arch.hvm_vmx.active_cpu;

    if ( cpu == -1 )
        return;

    if ( cpu == smp_processor_id() )
        return __vmx_clear_vmcs(v);

    on_selected_cpus(cpumask_of_cpu(cpu), __vmx_clear_vmcs, v, 1, 1);
}

static void vmx_load_vmcs(struct vcpu *v)
{
    __vmptrld(virt_to_maddr(v->arch.hvm_vmx.vmcs));
    v->arch.hvm_vmx.active_cpu = smp_processor_id();
}

void vmx_vmcs_enter(struct vcpu *v)
{
    /*
     * NB. We must *always* run an HVM VCPU on its own VMCS, except for
     * vmx_vmcs_enter/exit critical regions. This leads to some XXX TODOs XXX:
     *  1. Move construct_vmcs() much earlier, to domain creation or
     *     context initialisation.
     *  2. VMPTRLD as soon as we context-switch to a HVM VCPU.
     *  3. VMCS destruction needs to happen later (from domain_destroy()).
     * We can relax this a bit if a paused VCPU always commits its
     * architectural state to a software structure.
     */
    if ( v == current )
        return;

    vcpu_pause(v);
    spin_lock(&v->arch.hvm_vmx.vmcs_lock);

    vmx_clear_vmcs(v);
    vmx_load_vmcs(v);
}

void vmx_vmcs_exit(struct vcpu *v)
{
    if ( v == current )
        return;

    /* Don't confuse arch_vmx_do_resume (for @v or @current!) */
    vmx_clear_vmcs(v);
    if ( hvm_guest(current) )
        vmx_load_vmcs(current);

    spin_unlock(&v->arch.hvm_vmx.vmcs_lock);
    vcpu_unpause(v);
}

struct vmcs_struct *vmx_alloc_host_vmcs(void)
{
    return vmx_alloc_vmcs();
}

void vmx_free_host_vmcs(struct vmcs_struct *vmcs)
{
    vmx_free_vmcs(vmcs);
}

static inline int construct_vmcs_controls(struct arch_vmx_struct *arch_vmx)
{
    int error = 0;

    error |= __vmwrite(PIN_BASED_VM_EXEC_CONTROL,
                       MONITOR_PIN_BASED_EXEC_CONTROLS);

    error |= __vmwrite(VM_EXIT_CONTROLS, MONITOR_VM_EXIT_CONTROLS);

    error |= __vmwrite(VM_ENTRY_CONTROLS, MONITOR_VM_ENTRY_CONTROLS);

    error |= __vmwrite(IO_BITMAP_A, virt_to_maddr(arch_vmx->io_bitmap_a));
    error |= __vmwrite(IO_BITMAP_B, virt_to_maddr(arch_vmx->io_bitmap_b));

#ifdef CONFIG_X86_PAE
    /* On PAE bitmaps may in future be above 4GB. Write high words. */
    error |= __vmwrite(IO_BITMAP_A_HIGH,
                       (paddr_t)virt_to_maddr(arch_vmx->io_bitmap_a) >> 32);
    error |= __vmwrite(IO_BITMAP_B_HIGH,
                       (paddr_t)virt_to_maddr(arch_vmx->io_bitmap_b) >> 32);
#endif

    return error;
}

#define GUEST_LAUNCH_DS         0x08
#define GUEST_LAUNCH_CS         0x10
#define GUEST_SEGMENT_LIMIT     0xffffffff
#define HOST_SEGMENT_LIMIT      0xffffffff

struct host_execution_env {
    /* selectors */
    unsigned short ldtr_selector;
    unsigned short tr_selector;
    unsigned short ds_selector;
    unsigned short cs_selector;
    /* limits */
    unsigned short gdtr_limit;
    unsigned short ldtr_limit;
    unsigned short idtr_limit;
    unsigned short tr_limit;
    /* base */
    unsigned long gdtr_base;
    unsigned long ldtr_base;
    unsigned long idtr_base;
    unsigned long tr_base;
    unsigned long ds_base;
    unsigned long cs_base;
#ifdef __x86_64__
    unsigned long fs_base;
    unsigned long gs_base;
#endif
};

static void vmx_set_host_env(struct vcpu *v)
{
    unsigned int tr, cpu, error = 0;
    struct host_execution_env host_env;
    struct Xgt_desc_struct desc;

    cpu = smp_processor_id();
    __asm__ __volatile__ ("sidt  (%0) \n" :: "a"(&desc) : "memory");
    host_env.idtr_limit = desc.size;
    host_env.idtr_base = desc.address;
    error |= __vmwrite(HOST_IDTR_BASE, host_env.idtr_base);

    __asm__ __volatile__ ("sgdt  (%0) \n" :: "a"(&desc) : "memory");
    host_env.gdtr_limit = desc.size;
    host_env.gdtr_base = desc.address;
    error |= __vmwrite(HOST_GDTR_BASE, host_env.gdtr_base);

    __asm__ __volatile__ ("str  (%0) \n" :: "a"(&tr) : "memory");
    host_env.tr_selector = tr;
    host_env.tr_limit = sizeof(struct tss_struct);
    host_env.tr_base = (unsigned long) &init_tss[cpu];
    error |= __vmwrite(HOST_TR_SELECTOR, host_env.tr_selector);
    error |= __vmwrite(HOST_TR_BASE, host_env.tr_base);
    error |= __vmwrite(HOST_RSP, (unsigned long)get_stack_bottom());
}

static void vmx_do_launch(struct vcpu *v)
{
/* Update CR3, CR0, CR4, GDT, LDT, TR */
    unsigned int  error = 0;
    unsigned long cr0, cr4;

    if (v->vcpu_id == 0)
        hvm_setup_platform(v->domain);

    __asm__ __volatile__ ("mov %%cr0,%0" : "=r" (cr0) : );

    error |= __vmwrite(GUEST_CR0, cr0);
    cr0 &= ~X86_CR0_PG;
    error |= __vmwrite(CR0_READ_SHADOW, cr0);
    error |= __vmwrite(CPU_BASED_VM_EXEC_CONTROL,
                       MONITOR_CPU_BASED_EXEC_CONTROLS);
    v->arch.hvm_vcpu.u.vmx.exec_control = MONITOR_CPU_BASED_EXEC_CONTROLS;

    __asm__ __volatile__ ("mov %%cr4,%0" : "=r" (cr4) : );

    error |= __vmwrite(GUEST_CR4, cr4 & ~X86_CR4_PSE);
    cr4 &= ~(X86_CR4_PGE | X86_CR4_VMXE | X86_CR4_PAE);

    error |= __vmwrite(CR4_READ_SHADOW, cr4);

    hvm_stts(v);

    if(hvm_apic_support(v->domain))
        vlapic_init(v);

    vmx_set_host_env(v);
    init_timer(&v->arch.hvm_vcpu.hlt_timer, hlt_timer_fn, v, v->processor);

    error |= __vmwrite(GUEST_LDTR_SELECTOR, 0);
    error |= __vmwrite(GUEST_LDTR_BASE, 0);
    error |= __vmwrite(GUEST_LDTR_LIMIT, 0);

    error |= __vmwrite(GUEST_TR_BASE, 0);
    error |= __vmwrite(GUEST_TR_LIMIT, 0xff);

    shadow_update_paging_modes(v);
    printk("%s(): GUEST_CR3<=%08lx, HOST_CR3<=%08lx\n",
           __func__, v->arch.hvm_vcpu.hw_cr3, v->arch.cr3);
    __vmwrite(GUEST_CR3, v->arch.hvm_vcpu.hw_cr3);
    __vmwrite(HOST_CR3, v->arch.cr3);

    v->arch.schedule_tail = arch_vmx_do_resume;

    /* init guest tsc to start from 0 */
    hvm_set_guest_time(v, 0);
}

/*
 * Initially set the same environement as host.
 */
static inline int construct_init_vmcs_guest(cpu_user_regs_t *regs)
{
    int error = 0;
    union vmcs_arbytes arbytes;
    unsigned long dr7;
    unsigned long eflags;

    /* MSR */
    error |= __vmwrite(VM_EXIT_MSR_LOAD_ADDR, 0);
    error |= __vmwrite(VM_EXIT_MSR_STORE_ADDR, 0);

    error |= __vmwrite(VM_EXIT_MSR_STORE_COUNT, 0);
    error |= __vmwrite(VM_EXIT_MSR_LOAD_COUNT, 0);
    error |= __vmwrite(VM_ENTRY_MSR_LOAD_COUNT, 0);
    /* interrupt */
    error |= __vmwrite(VM_ENTRY_INTR_INFO_FIELD, 0);
    /* mask */
    error |= __vmwrite(CR0_GUEST_HOST_MASK, -1UL);
    error |= __vmwrite(CR4_GUEST_HOST_MASK, -1UL);

    error |= __vmwrite(PAGE_FAULT_ERROR_CODE_MASK, 0);
    error |= __vmwrite(PAGE_FAULT_ERROR_CODE_MATCH, 0);

    /* TSC */
    error |= __vmwrite(CR3_TARGET_COUNT, 0);

    /* Guest Selectors */
    error |= __vmwrite(GUEST_ES_SELECTOR, GUEST_LAUNCH_DS);
    error |= __vmwrite(GUEST_SS_SELECTOR, GUEST_LAUNCH_DS);
    error |= __vmwrite(GUEST_DS_SELECTOR, GUEST_LAUNCH_DS);
    error |= __vmwrite(GUEST_FS_SELECTOR, GUEST_LAUNCH_DS);
    error |= __vmwrite(GUEST_GS_SELECTOR, GUEST_LAUNCH_DS);
    error |= __vmwrite(GUEST_CS_SELECTOR, GUEST_LAUNCH_CS);

    /* Guest segment bases */
    error |= __vmwrite(GUEST_ES_BASE, 0);
    error |= __vmwrite(GUEST_SS_BASE, 0);
    error |= __vmwrite(GUEST_DS_BASE, 0);
    error |= __vmwrite(GUEST_FS_BASE, 0);
    error |= __vmwrite(GUEST_GS_BASE, 0);
    error |= __vmwrite(GUEST_CS_BASE, 0);

    /* Guest segment Limits */
    error |= __vmwrite(GUEST_ES_LIMIT, GUEST_SEGMENT_LIMIT);
    error |= __vmwrite(GUEST_SS_LIMIT, GUEST_SEGMENT_LIMIT);
    error |= __vmwrite(GUEST_DS_LIMIT, GUEST_SEGMENT_LIMIT);
    error |= __vmwrite(GUEST_FS_LIMIT, GUEST_SEGMENT_LIMIT);
    error |= __vmwrite(GUEST_GS_LIMIT, GUEST_SEGMENT_LIMIT);
    error |= __vmwrite(GUEST_CS_LIMIT, GUEST_SEGMENT_LIMIT);

    /* Guest segment AR bytes */
    arbytes.bytes = 0;
    arbytes.fields.seg_type = 0x3;          /* type = 3 */
    arbytes.fields.s = 1;                   /* code or data, i.e. not system */
    arbytes.fields.dpl = 0;                 /* DPL = 3 */
    arbytes.fields.p = 1;                   /* segment present */
    arbytes.fields.default_ops_size = 1;    /* 32-bit */
    arbytes.fields.g = 1;
    arbytes.fields.null_bit = 0;            /* not null */

    error |= __vmwrite(GUEST_ES_AR_BYTES, arbytes.bytes);
    error |= __vmwrite(GUEST_SS_AR_BYTES, arbytes.bytes);
    error |= __vmwrite(GUEST_DS_AR_BYTES, arbytes.bytes);
    error |= __vmwrite(GUEST_FS_AR_BYTES, arbytes.bytes);
    error |= __vmwrite(GUEST_GS_AR_BYTES, arbytes.bytes);

    arbytes.fields.seg_type = 0xb;          /* type = 0xb */
    error |= __vmwrite(GUEST_CS_AR_BYTES, arbytes.bytes);

    /* Guest GDT */
    error |= __vmwrite(GUEST_GDTR_BASE, 0);
    error |= __vmwrite(GUEST_GDTR_LIMIT, 0);

    /* Guest IDT */
    error |= __vmwrite(GUEST_IDTR_BASE, 0);
    error |= __vmwrite(GUEST_IDTR_LIMIT, 0);

    /* Guest LDT & TSS */
    arbytes.fields.s = 0;                   /* not code or data segement */
    arbytes.fields.seg_type = 0x2;          /* LTD */
    arbytes.fields.default_ops_size = 0;    /* 16-bit */
    arbytes.fields.g = 0;
    error |= __vmwrite(GUEST_LDTR_AR_BYTES, arbytes.bytes);

    arbytes.fields.seg_type = 0xb;          /* 32-bit TSS (busy) */
    error |= __vmwrite(GUEST_TR_AR_BYTES, arbytes.bytes);
    /* CR3 is set in vmx_final_setup_guest */

    error |= __vmwrite(GUEST_RSP, 0);
    error |= __vmwrite(GUEST_RIP, regs->eip);

    /* Guest EFLAGS */
    eflags = regs->eflags & ~HVM_EFLAGS_RESERVED_0; /* clear 0s */
    eflags |= HVM_EFLAGS_RESERVED_1; /* set 1s */
    error |= __vmwrite(GUEST_RFLAGS, eflags);

    error |= __vmwrite(GUEST_INTERRUPTIBILITY_INFO, 0);
    __asm__ __volatile__ ("mov %%dr7, %0\n" : "=r" (dr7));
    error |= __vmwrite(GUEST_DR7, dr7);
    error |= __vmwrite(VMCS_LINK_POINTER, ~0UL);
#if defined(__i386__)
    error |= __vmwrite(VMCS_LINK_POINTER_HIGH, ~0UL);
#endif

    return error;
}

static inline int construct_vmcs_host(void)
{
    int error = 0;
#ifdef __x86_64__
    unsigned long fs_base;
    unsigned long gs_base;
#endif
    unsigned long crn;

    /* Host Selectors */
    error |= __vmwrite(HOST_ES_SELECTOR, __HYPERVISOR_DS);
    error |= __vmwrite(HOST_SS_SELECTOR, __HYPERVISOR_DS);
    error |= __vmwrite(HOST_DS_SELECTOR, __HYPERVISOR_DS);
#if defined(__i386__)
    error |= __vmwrite(HOST_FS_SELECTOR, __HYPERVISOR_DS);
    error |= __vmwrite(HOST_GS_SELECTOR, __HYPERVISOR_DS);
    error |= __vmwrite(HOST_FS_BASE, 0);
    error |= __vmwrite(HOST_GS_BASE, 0);

#else
    rdmsrl(MSR_FS_BASE, fs_base);
    rdmsrl(MSR_GS_BASE, gs_base);
    error |= __vmwrite(HOST_FS_BASE, fs_base);
    error |= __vmwrite(HOST_GS_BASE, gs_base);

#endif
    error |= __vmwrite(HOST_CS_SELECTOR, __HYPERVISOR_CS);

    __asm__ __volatile__ ("mov %%cr0,%0" : "=r" (crn) : );
    error |= __vmwrite(HOST_CR0, crn); /* same CR0 */

    /* CR3 is set in vmx_final_setup_hostos */
    __asm__ __volatile__ ("mov %%cr4,%0" : "=r" (crn) : );
    error |= __vmwrite(HOST_CR4, crn);

    error |= __vmwrite(HOST_RIP, (unsigned long) vmx_asm_vmexit_handler);
#ifdef __x86_64__
    /* TBD: support cr8 for 64-bit guest */
    __vmwrite(VIRTUAL_APIC_PAGE_ADDR, 0);
    __vmwrite(TPR_THRESHOLD, 0);
    __vmwrite(SECONDARY_VM_EXEC_CONTROL, 0);
#endif

    return error;
}

/*
 * the working VMCS pointer has been set properly
 * just before entering this function.
 */
static int construct_vmcs(struct vcpu *v,
                          cpu_user_regs_t *regs)
{
    struct arch_vmx_struct *arch_vmx = &v->arch.hvm_vmx;
    int error;

    if ( (error = construct_vmcs_controls(arch_vmx)) ) {
        printk("construct_vmcs: construct_vmcs_controls failed.\n");
        return error;
    }

    /* host selectors */
    if ( (error = construct_vmcs_host()) ) {
        printk("construct_vmcs: construct_vmcs_host failed.\n");
        return error;
    }

    /* guest selectors */
    if ( (error = construct_init_vmcs_guest(regs)) ) {
        printk("construct_vmcs: construct_vmcs_guest failed.\n");
        return error;
    }

    if ( (error = __vmwrite(EXCEPTION_BITMAP,
                            MONITOR_DEFAULT_EXCEPTION_BITMAP)) ) {
        printk("construct_vmcs: setting exception bitmap failed.\n");
        return error;
    }

    if ( regs->eflags & EF_TF )
        error = __vm_set_bit(EXCEPTION_BITMAP, EXCEPTION_BITMAP_DB);
    else
        error = __vm_clear_bit(EXCEPTION_BITMAP, EXCEPTION_BITMAP_DB);

    return error;
}

int vmx_create_vmcs(struct vcpu *v)
{
    if ( (v->arch.hvm_vmx.vmcs = vmx_alloc_vmcs()) == NULL )
        return -ENOMEM;
    __vmx_clear_vmcs(v);
    return 0;
}

void vmx_destroy_vmcs(struct vcpu *v)
{
    struct arch_vmx_struct *arch_vmx = &v->arch.hvm_vmx;

    if ( arch_vmx->vmcs == NULL )
        return;

    vmx_clear_vmcs(v);

    free_xenheap_pages(arch_vmx->io_bitmap_a, IO_BITMAP_ORDER);
    free_xenheap_pages(arch_vmx->io_bitmap_b, IO_BITMAP_ORDER);

    arch_vmx->io_bitmap_a = NULL;
    arch_vmx->io_bitmap_b = NULL;

    vmx_free_vmcs(arch_vmx->vmcs);
    arch_vmx->vmcs = NULL;
}

void vm_launch_fail(unsigned long eflags)
{
    unsigned long error;
    __vmread(VM_INSTRUCTION_ERROR, &error);
    printk("<vm_launch_fail> error code %lx\n", error);
    __hvm_bug(guest_cpu_user_regs());
}

void vm_resume_fail(unsigned long eflags)
{
    unsigned long error;
    __vmread(VM_INSTRUCTION_ERROR, &error);
    printk("<vm_resume_fail> error code %lx\n", error);
    __hvm_bug(guest_cpu_user_regs());
}

void arch_vmx_do_resume(struct vcpu *v)
{
    if ( v->arch.hvm_vmx.active_cpu == smp_processor_id() )
    {
        vmx_load_vmcs(v);
    }
    else
    {
        vmx_clear_vmcs(v);
        vmx_load_vmcs(v);
        vmx_migrate_timers(v);
        vmx_set_host_env(v);
    }

    hvm_do_resume(v);
    reset_stack_and_jump(vmx_asm_do_vmentry);
}

void arch_vmx_do_launch(struct vcpu *v)
{
    cpu_user_regs_t *regs = &current->arch.guest_context.user_regs;

    vmx_load_vmcs(v);

    if ( construct_vmcs(v, regs) < 0 )
    {
        if ( v->vcpu_id == 0 ) {
            printk("Failed to construct VMCS for BSP.\n");
        } else {
            printk("Failed to construct VMCS for AP %d.\n", v->vcpu_id);
        }
        domain_crash_synchronous();
    }

    vmx_do_launch(v);
    reset_stack_and_jump(vmx_asm_do_vmentry);
}


/* Dump a section of VMCS */
static void print_section(char *header, uint32_t start, 
                          uint32_t end, int incr)
{
    uint32_t addr, j;
    unsigned long val;
    int code;
    char *fmt[4] = {"0x%04lx ", "0x%016lx ", "0x%08lx ", "0x%016lx "};
    char *err[4] = {"------ ", "------------------ ", 
                    "---------- ", "------------------ "};

    /* Find width of the field (encoded in bits 14:13 of address) */
    code = (start>>13)&3;

    if (header)
        printk("\t %s", header);

    for (addr=start, j=0; addr<=end; addr+=incr, j++) {

        if (!(j&3))
            printk("\n\t\t0x%08x: ", addr);

        if (!__vmread(addr, &val))
            printk(fmt[code], val);
        else
            printk("%s", err[code]);
    }

    printk("\n");
}

/* Dump current VMCS */
void vmcs_dump_vcpu(void)
{
    print_section("16-bit Guest-State Fields", 0x800, 0x80e, 2);
    print_section("16-bit Host-State Fields", 0xc00, 0xc0c, 2);
    print_section("64-bit Control Fields", 0x2000, 0x2013, 1);
    print_section("64-bit Guest-State Fields", 0x2800, 0x2803, 1);
    print_section("32-bit Control Fields", 0x4000, 0x401c, 2);
    print_section("32-bit RO Data Fields", 0x4400, 0x440e, 2);
    print_section("32-bit Guest-State Fields", 0x4800, 0x482a, 2);
    print_section("32-bit Host-State Fields", 0x4c00, 0x4c00, 2);
    print_section("Natural 64-bit Control Fields", 0x6000, 0x600e, 2);
    print_section("64-bit RO Data Fields", 0x6400, 0x640A, 2);
    print_section("Natural 64-bit Guest-State Fields", 0x6800, 0x6826, 2);
    print_section("Natural 64-bit Host-State Fields", 0x6c00, 0x6c16, 2);
}


static void vmcs_dump(unsigned char ch)
{
    struct domain *d;
    struct vcpu *v;
    
    printk("*********** VMCS Areas **************\n");
    for_each_domain(d) {
        printk("\n>>> Domain %d <<<\n", d->domain_id);
        for_each_vcpu(d, v) {

            /* 
             * Presumably, if a domain is not an HVM guest,
             * the very first CPU will not pass this test
             */
            if (!hvm_guest(v)) {
                printk("\t\tNot HVM guest\n");
                break;
            }
            printk("\tVCPU %d\n", v->vcpu_id);

            vmx_vmcs_enter(v);
            vmcs_dump_vcpu();
            vmx_vmcs_exit(v);
        }
    }

    printk("**************************************\n");
}

void setup_vmcs_dump(void)
{
    register_keyhandler('v', vmcs_dump, "dump Intel's VMCS");
}


/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
