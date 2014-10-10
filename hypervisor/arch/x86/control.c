/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) Siemens AG, 2013
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <jailhouse/control.h>
#include <jailhouse/pci.h>
#include <jailhouse/printk.h>
#include <jailhouse/processor.h>
#include <asm/apic.h>
#include <asm/control.h>
#include <asm/ioapic.h>
#include <asm/iommu.h>
#include <asm/vcpu.h>

struct exception_frame {
	u64 vector;
	u64 error;
	u64 rip;
	u64 cs;
	u64 flags;
	u64 rsp;
	u64 ss;
};

int arch_cell_create(struct cell *cell)
{
	int err;

	err = vcpu_cell_init(cell);
	if (err)
		return err;

	err = iommu_cell_init(cell);
	if (err)
		goto error_vm_exit;

	err = pci_cell_init(cell);
	if (err)
		goto error_iommu_exit;

	ioapic_cell_init(cell);

	cell->comm_page.comm_region.pm_timer_address =
		system_config->platform_info.x86.pm_timer_address;

	return 0;

error_iommu_exit:
	iommu_cell_exit(cell);
error_vm_exit:
	vcpu_cell_exit(cell);
	return err;
}

int arch_map_memory_region(struct cell *cell,
			   const struct jailhouse_memory *mem)
{
	int err;

	err = vcpu_map_memory_region(cell, mem);
	if (err)
		return err;

	err = iommu_map_memory_region(cell, mem);
	if (err)
		vcpu_unmap_memory_region(cell, mem);
	return err;
}

int arch_unmap_memory_region(struct cell *cell,
			     const struct jailhouse_memory *mem)
{
	int err;

	err = iommu_unmap_memory_region(cell, mem);
	if (err)
		return err;

	return vcpu_unmap_memory_region(cell, mem);
}

void arch_cell_destroy(struct cell *cell)
{
	ioapic_cell_exit(cell);
	pci_cell_exit(cell);
	iommu_cell_exit(cell);
	vcpu_cell_exit(cell);
}

/* all root cell CPUs (except the calling one) have to be suspended */
void arch_config_commit(struct cell *cell_added_removed)
{
	unsigned int cpu, current_cpu = this_cpu_id();

	for_each_cpu_except(cpu, root_cell.cpu_set, current_cpu)
		per_cpu(cpu)->flush_vcpu_caches = true;

	if (cell_added_removed && cell_added_removed != &root_cell)
		for_each_cpu_except(cpu, cell_added_removed->cpu_set,
				    current_cpu)
			per_cpu(cpu)->flush_vcpu_caches = true;

	vcpu_tlb_flush();

	iommu_config_commit(cell_added_removed);
	pci_config_commit(cell_added_removed);
	ioapic_config_commit(cell_added_removed);
}

void arch_shutdown(void)
{
	pci_prepare_handover();
	ioapic_prepare_handover();

	iommu_shutdown();
	pci_shutdown();
	ioapic_shutdown();
}

void arch_suspend_cpu(unsigned int cpu_id)
{
	struct per_cpu *target_data = per_cpu(cpu_id);
	bool target_suspended;

	spin_lock(&target_data->control_lock);

	target_data->suspend_cpu = true;
	target_suspended = target_data->cpu_suspended;

	spin_unlock(&target_data->control_lock);

	if (!target_suspended) {
		apic_send_nmi_ipi(target_data);

		while (!target_data->cpu_suspended)
			cpu_relax();
	}
}

void arch_resume_cpu(unsigned int cpu_id)
{
	/* make any state changes visible before releasing the CPU */
	memory_barrier();

	per_cpu(cpu_id)->suspend_cpu = false;
}

void arch_reset_cpu(unsigned int cpu_id)
{
	per_cpu(cpu_id)->sipi_vector = APIC_BSP_PSEUDO_SIPI;

	arch_resume_cpu(cpu_id);
}

void arch_park_cpu(unsigned int cpu_id)
{
	per_cpu(cpu_id)->init_signaled = true;

	arch_resume_cpu(cpu_id);
}

void arch_shutdown_cpu(unsigned int cpu_id)
{
	arch_suspend_cpu(cpu_id);
	per_cpu(cpu_id)->shutdown_cpu = true;
	arch_resume_cpu(cpu_id);
}

void x86_send_init_sipi(unsigned int cpu_id, enum x86_init_sipi type,
			int sipi_vector)
{
	struct per_cpu *target_data = per_cpu(cpu_id);
	bool send_nmi = false;

	spin_lock(&target_data->control_lock);

	if (type == X86_INIT) {
		if (!target_data->wait_for_sipi) {
			target_data->init_signaled = true;
			send_nmi = true;
		}
	} else if (target_data->wait_for_sipi) {
		target_data->sipi_vector = sipi_vector;
		send_nmi = true;
	}

	spin_unlock(&target_data->control_lock);

	if (send_nmi)
		apic_send_nmi_ipi(target_data);
}

/* control_lock has to be held */
static void x86_enter_wait_for_sipi(struct per_cpu *cpu_data)
{
	cpu_data->init_signaled = false;
	cpu_data->wait_for_sipi = true;
}

int x86_handle_events(struct per_cpu *cpu_data)
{
	int sipi_vector = -1;

	spin_lock(&cpu_data->control_lock);

	do {
		if (cpu_data->init_signaled && !cpu_data->suspend_cpu) {
			x86_enter_wait_for_sipi(cpu_data);
			sipi_vector = -1;
			break;
		}

		cpu_data->cpu_suspended = true;

		spin_unlock(&cpu_data->control_lock);

		while (cpu_data->suspend_cpu)
			cpu_relax();

		if (cpu_data->shutdown_cpu) {
			apic_clear(cpu_data);
			vcpu_exit(cpu_data);
			asm volatile("1: hlt; jmp 1b");
		}

		spin_lock(&cpu_data->control_lock);

		cpu_data->cpu_suspended = false;

		if (cpu_data->sipi_vector >= 0) {
			if (!cpu_data->failed) {
				cpu_data->wait_for_sipi = false;
				sipi_vector = cpu_data->sipi_vector;
			}
			cpu_data->sipi_vector = -1;
		}
	} while (cpu_data->init_signaled);

	if (cpu_data->flush_vcpu_caches) {
		cpu_data->flush_vcpu_caches = false;
		vcpu_tlb_flush();
	}

	spin_unlock(&cpu_data->control_lock);

	/* wait_for_sipi is only modified on this CPU, so checking outside of
	 * control_lock is fine */
	if (cpu_data->wait_for_sipi)
		vcpu_park(cpu_data);
	else if (sipi_vector >= 0)
		apic_clear(cpu_data);

	return sipi_vector;
}

void x86_exception_handler(struct exception_frame *frame)
{
	panic_printk("FATAL: Jailhouse triggered exception #%d\n",
		     frame->vector);
	if (frame->error != -1)
		panic_printk("Error code: %x\n", frame->error);
	panic_printk("Physical CPU ID: %d\n", phys_processor_id());
	panic_printk("RIP: %p RSP: %p FLAGS: %x\n", frame->rip, frame->rsp,
		     frame->flags);
	if (frame->vector == PF_VECTOR)
		panic_printk("CR2: %p\n", read_cr2());

	panic_stop();
}

void arch_panic_stop(void)
{
	asm volatile("1: hlt; jmp 1b");
	__builtin_unreachable();
}

void arch_panic_park(void)
{
	struct per_cpu *cpu_data = this_cpu_data();

	spin_lock(&cpu_data->control_lock);
	x86_enter_wait_for_sipi(cpu_data);
	spin_unlock(&cpu_data->control_lock);

	vcpu_park(cpu_data);
}
