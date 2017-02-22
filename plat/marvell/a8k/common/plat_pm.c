/*
 * ***************************************************************************
 * Copyright (C) 2016 Marvell International Ltd.
 * ***************************************************************************
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of Marvell nor the names of its contributors may be used
 * to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ***************************************************************************
 */

#include <plat_marvell.h>
#include <gicv2.h>
#include <mmio.h>
#include <debug.h>
#include <delay_timer.h>

#ifdef SCP_IMAGE
#include <bakery_lock.h>
#include <platform.h>
#include <mss_pm_ipc.h>
#include <plat_pm_trace.h>
#endif

#define MVEBU_PRIVATE_UID_REG		0x30
#define MVEBU_RFU_GLOBL_SW_RST		0x84
#define MVEBU_CCU_RVBAR(i)		(MVEBU_REGS_BASE + 0x640 + (i * 4))
#define MVEBU_CCU_CPU_UN_RESET		(MVEBU_REGS_BASE + 0x650)

#define MPIDR_CPU_GET(mpidr)		((mpidr) & MPIDR_CPU_MASK)
#define MPIDR_CLUSTER_GET(mpidr)	MPIDR_AFFLVL1_VAL((mpidr))

#ifdef SCP_IMAGE
/* this lock synchronize AP multiple cores execution with MSS */
DEFINE_BAKERY_LOCK(pm_sys_lock);
#endif


/* AP806 CPU power down /power up definitions */
enum CPU_ID {
	CPU0,
	CPU1,
	CPU2,
	CPU3
};

#define CPUS_PER_CLUSTER		2
#define REG_WR_VALIDATE_TIMEOUT		(2000)

#define FEATURE_DISABLE_STATUS_REG			(MVEBU_REGS_BASE + 0x6F8230)
#define FEATURE_DISABLE_STATUS_CPU_CLUSTER_OFFSET	4
#define FEATURE_DISABLE_STATUS_CPU_CLUSTER_MASK		(0x1 << FEATURE_DISABLE_STATUS_CPU_CLUSTER_OFFSET)

#define PWRC_CPUN_CR_REG(cpu_id)		(MVEBU_REGS_BASE + 0x680000 + (cpu_id * 0x10))
#define PWRC_CPUN_CR_PWR_DN_RQ_OFFSET		0
#define PWRC_CPUN_CR_PWR_DN_RQ_MASK		(0x1 << PWRC_CPUN_CR_PWR_DN_RQ_OFFSET)
#define PWRC_CPUN_CR_ISO_ENABLE_OFFSET		16
#define PWRC_CPUN_CR_ISO_ENABLE_MASK		(0x1 << PWRC_CPUN_CR_ISO_ENABLE_OFFSET)
#define PWRC_CPUN_CR_LDO_BYPASS_RDY_OFFSET	31
#define PWRC_CPUN_CR_LDO_BYPASS_RDY_MASK	(0x1 << PWRC_CPUN_CR_LDO_BYPASS_RDY_OFFSET)

#define CCU_B_PRCRN_REG(cpu_id)			(MVEBU_REGS_BASE + 0x1A50 + \
						((cpu_id / 2) * (0x400)) + ((cpu_id % 2) * 4))
#define CCU_B_PRCRN_CPUPORESET_STATIC_OFFSET	0
#define CCU_B_PRCRN_CPUPORESET_STATIC_MASK	(0x1 << CCU_B_PRCRN_CPUPORESET_STATIC_OFFSET)

/*
 * Power down CPU:
 * Used to reduce power consumption, and avoid SoC unnecessary temperature rise.
 */
int plat_marvell_cpu_powerdown(int cpu_id)
{
	uint32_t	reg_val;
	int		exit_loop = REG_WR_VALIDATE_TIMEOUT;

	INFO("Powering down CPU%d\n", cpu_id);

	/* 1. Isolation enable */
	reg_val = mmio_read_32(PWRC_CPUN_CR_REG(cpu_id));
	reg_val |= 0x1 << PWRC_CPUN_CR_ISO_ENABLE_OFFSET;
	mmio_write_32(PWRC_CPUN_CR_REG(cpu_id), reg_val);

	/* 2. Read and check Isolation enabled - verify bit set to 1 */
	do {
		reg_val = mmio_read_32(PWRC_CPUN_CR_REG(cpu_id));
		exit_loop--;
	} while (!(reg_val & (0x1 << PWRC_CPUN_CR_ISO_ENABLE_OFFSET)) && exit_loop > 0);

	/* 3. Switch off CPU power */
	reg_val = mmio_read_32(PWRC_CPUN_CR_REG(cpu_id));
	reg_val &= ~PWRC_CPUN_CR_PWR_DN_RQ_MASK;
	mmio_write_32(PWRC_CPUN_CR_REG(cpu_id), reg_val);

	/* 4. Read and check Switch Off - verify bit set to 0 */
	exit_loop = REG_WR_VALIDATE_TIMEOUT;
	do {
		reg_val = mmio_read_32(PWRC_CPUN_CR_REG(cpu_id));
		exit_loop--;
	} while (reg_val & PWRC_CPUN_CR_PWR_DN_RQ_MASK && exit_loop > 0);

	if (exit_loop <= 0)
		goto cpu_poweroff_error;

	/* 5. De-Assert power ready */
	reg_val = mmio_read_32(PWRC_CPUN_CR_REG(cpu_id));
	reg_val &= ~PWRC_CPUN_CR_LDO_BYPASS_RDY_MASK;
	mmio_write_32(PWRC_CPUN_CR_REG(cpu_id), reg_val);

	/* 6. Assert CPU POR reset */
	reg_val = mmio_read_32(CCU_B_PRCRN_REG(cpu_id));
	reg_val &= ~CCU_B_PRCRN_CPUPORESET_STATIC_MASK;
	mmio_write_32(CCU_B_PRCRN_REG(cpu_id), reg_val);

	/* 7. Read and poll on Validate the CPU is out of reset */
	exit_loop = REG_WR_VALIDATE_TIMEOUT;
	do {
		reg_val = mmio_read_32(CCU_B_PRCRN_REG(cpu_id));
		exit_loop--;
	} while (reg_val & CCU_B_PRCRN_CPUPORESET_STATIC_MASK && exit_loop > 0);

	if (exit_loop <= 0)
		goto cpu_poweroff_error;

	INFO("Successfully powered down CPU%d\n", cpu_id);

	return 0;

cpu_poweroff_error:
	ERROR("ERROR: Can't power down CPU%d\n" , cpu_id);
	return -1;
}

/*
 * Power down CPUs 1-3 at early boot stage,
 * to reduce power consumption and SoC temperature.
 * This is triggered by BLE prior to DDR initialization.
 *
 * Note:
 * All CPUs will be powered up by plat_marvell_cpu_powerup on Linux boot stage,
 * which is triggered by PSCI ops (pwr_domain_on).
 */
int plat_marvell_early_cpu_powerdown(void)
{
	uint32_t cpu_cluster_status = mmio_read_32(FEATURE_DISABLE_STATUS_REG)
						& FEATURE_DISABLE_STATUS_CPU_CLUSTER_MASK;
	/* if cpu_cluster_status bit is set, that means we have only single cluster */
	int cluster_count = cpu_cluster_status ? 1 : 2;

	INFO("Powering off unused CPUs\n");

	/* CPU1 is in AP806 cluster-0, which always exists - so power it down */
	if (plat_marvell_cpu_powerdown(CPU1) == -1)
		return -1;

	/*
	 * CPU2-3 are in AP806 2nd cluster (cluster-1), which doesn't exists in dual-core systems.
	 * so need to check if we have dual-core (single cluster) or quad-code (2 clusters)
	 */
	if (cluster_count == 2) {
		/* CPU2-3 are part of 2nd cluster */
		if (plat_marvell_cpu_powerdown(CPU2) == -1)
			return -1;
		if (plat_marvell_cpu_powerdown(CPU3) == -1)
			return -1;
	}

	return 0;
}

/*
 * Power up CPU - part of Linux boot stage
 */
int plat_marvell_cpu_powerup(u_register_t mpidr)
{
	uint32_t	reg_val;
	int	cpu_id = MPIDR_CPU_GET(mpidr), cluster = MPIDR_CLUSTER_GET(mpidr);
	int	exit_loop = REG_WR_VALIDATE_TIMEOUT;

	/* calculate absolute CPU ID */
	cpu_id = cluster * CPUS_PER_CLUSTER + cpu_id;

	INFO("Powering on CPU%d\n", cpu_id);

	/* 1. Switch CPU power ON */
	reg_val = mmio_read_32(PWRC_CPUN_CR_REG(cpu_id));
	reg_val |= 0x1 << PWRC_CPUN_CR_PWR_DN_RQ_OFFSET;
	mmio_write_32(PWRC_CPUN_CR_REG(cpu_id), reg_val);

	/* 2. Wait for CPU on, up to 100 uSec: */
	udelay(100);

	/* 3. Assert power ready */
	reg_val = mmio_read_32(PWRC_CPUN_CR_REG(cpu_id));
	reg_val |= 0x1 << PWRC_CPUN_CR_LDO_BYPASS_RDY_OFFSET;
	mmio_write_32(PWRC_CPUN_CR_REG(cpu_id), reg_val);

	/* 4. Read & Validate power ready - used in order to generate 16 Host CPU cycles */
	do {
		reg_val = mmio_read_32(PWRC_CPUN_CR_REG(cpu_id));
		exit_loop--;
	} while (!(reg_val & (0x1 << PWRC_CPUN_CR_LDO_BYPASS_RDY_OFFSET)) && exit_loop > 0);

	if (exit_loop <= 0)
		goto cpu_poweron_error;

	/* 5. Isolation disable */
	reg_val = mmio_read_32(PWRC_CPUN_CR_REG(cpu_id));
	reg_val &= ~PWRC_CPUN_CR_ISO_ENABLE_MASK;
	mmio_write_32(PWRC_CPUN_CR_REG(cpu_id), reg_val);

	/* 6. Read and check Isolation enabled - verify bit set to 1 */
	exit_loop = REG_WR_VALIDATE_TIMEOUT;
	do {
		reg_val = mmio_read_32(PWRC_CPUN_CR_REG(cpu_id));
		exit_loop--;
	} while ((reg_val & (0x1 << PWRC_CPUN_CR_ISO_ENABLE_OFFSET)) && exit_loop > 0);

	/* 7. De Assert CPU POR reset & Core reset */
	reg_val = mmio_read_32(CCU_B_PRCRN_REG(cpu_id));
	reg_val |= 0x1 << CCU_B_PRCRN_CPUPORESET_STATIC_OFFSET;
	mmio_write_32(CCU_B_PRCRN_REG(cpu_id), reg_val);

	/* 8. Read & Validate CPU POR reset */
	exit_loop = REG_WR_VALIDATE_TIMEOUT;
	do {
		reg_val = mmio_read_32(CCU_B_PRCRN_REG(cpu_id));
		exit_loop--;
	} while (!(reg_val & (0x1 << CCU_B_PRCRN_CPUPORESET_STATIC_OFFSET)) && exit_loop > 0);

	if (exit_loop <= 0)
		goto cpu_poweron_error;

	INFO("Successfully powered on CPU%d\n", cpu_id);

	return 0;

cpu_poweron_error:
	ERROR("ERROR: Can't power up CPU%d\n" , cpu_id);
	return -1;
}


int plat_marvell_cpu_on(u_register_t mpidr)
{
	int cpu_id;
	int cluster;

	/* Set barierr */
	__asm__ volatile("dsb     sy");

	/* Get cpu number - use CPU ID */
	cpu_id =  MPIDR_CPU_GET(mpidr);

	/* Get cluster number - use affinity level 1 */
	cluster = MPIDR_CLUSTER_GET(mpidr);

	/* Set CPU private UID */
	mmio_write_32(MVEBU_REGS_BASE + MVEBU_PRIVATE_UID_REG, cluster + 0x4);

	/* Set the cpu start address to BL1 entry point (align to 0x10000) */
	mmio_write_32(MVEBU_CCU_RVBAR(0) + (cpu_id << 2),
		      PLAT_MARVELL_CPU_ENTRY_ADDR >> 16);

	/* Get the cpu out of reset */
	mmio_write_32(MVEBU_CCU_CPU_UN_RESET + (cpu_id << 2), 0x10001);

	return 0;
}

void plat_marvell_system_reset(void)
{
	mmio_write_32(MVEBU_RFU_BASE + MVEBU_RFU_GLOBL_SW_RST, 0x0);
}

/*******************************************************************************
 * A8K handler called to check the validity of the power state
 * parameter.
 ******************************************************************************/
int a8k_validate_power_state(unsigned int power_state,
			    psci_power_state_t *req_state)
{
	int pstate = psci_get_pstate_type(power_state);
	int pwr_lvl = psci_get_pstate_pwrlvl(power_state);
	int i;

	if (pwr_lvl > PLAT_MAX_PWR_LVL)
		return PSCI_E_INVALID_PARAMS;

	/* Sanity check the requested state */
	if (pstate == PSTATE_TYPE_STANDBY) {
		/*
		 * It's possible to enter standby only on power level 0
		 * Ignore any other power level.
		 */
		if (pwr_lvl != MARVELL_PWR_LVL0)
			return PSCI_E_INVALID_PARAMS;

		req_state->pwr_domain_state[MARVELL_PWR_LVL0] =
					MARVELL_LOCAL_STATE_RET;
	} else {
		for (i = MARVELL_PWR_LVL0; i <= pwr_lvl; i++)
			req_state->pwr_domain_state[i] =
					MARVELL_LOCAL_STATE_OFF;
	}

	/*
	 * We expect the 'state id' to be zero.
	 */
	if (psci_get_pstate_id(power_state))
		return PSCI_E_INVALID_PARAMS;

	return PSCI_E_SUCCESS;
}

/*******************************************************************************
 * A8K handler called when a CPU is about to enter standby.
 ******************************************************************************/
void a8k_cpu_standby(plat_local_state_t cpu_state)
{
	ERROR("a8k_cpu_standby needs to be implemented\n");
	panic();
}

/*******************************************************************************
 * A8K handler called when a power domain is about to be turned on. The
 * mpidr determines the CPU to be turned on.
 ******************************************************************************/
int a8k_pwr_domain_on(u_register_t mpidr)
{
	/* Power up CPU (CPUs 1-3 are powered off at start of BLE) */
	plat_marvell_cpu_powerup(mpidr);

#ifdef SCP_IMAGE
	unsigned int target = ((mpidr & 0xFF) + (((mpidr >> 8) & 0xFF) * 2));

	/*
	 * pm system synchronization - used to synchronize
	 * multiple core access to MSS
	 */
	bakery_lock_get(&pm_sys_lock);

	/* send CPU ON IPC Message to MSS */
	mss_pm_ipc_msg_send(target, PM_IPC_MSG_CPU_ON, 0);

	/* trigger IPC message to MSS */
	mss_pm_ipc_msg_trigger();

	/* pm system synchronization */
	bakery_lock_release(&pm_sys_lock);

	/* trace message */
	PM_TRACE((TRACE_PWR_DOMAIN_ON | target), plat_my_core_pos());
#else
	/* proprietary CPU ON exection flow */
	plat_marvell_cpu_on(mpidr);
#endif /* SCP_IMAGE */

	return 0;
}

/*******************************************************************************
 * A8K handler called to validate the entry point.
 ******************************************************************************/
int a8k_validate_ns_entrypoint(uintptr_t entrypoint)
{
	return PSCI_E_SUCCESS;
}

/*******************************************************************************
 * A8K handler called when a power domain is about to be turned off. The
 * target_state encodes the power state that each level should transition to.
 ******************************************************************************/
void a8k_pwr_domain_off(const psci_power_state_t *target_state)
{
#ifdef SCP_IMAGE
	unsigned int idx = plat_my_core_pos();

	/* Prevent interrupts from spuriously waking up this cpu */
	gicv2_cpuif_disable();

	/*
	 * pm system synchronization - used to synchronize
	 * multiple core access to MSS
	 */
	bakery_lock_get(&pm_sys_lock);

	/* send CPU OFF IPC Message to MSS */
	mss_pm_ipc_msg_send(idx, PM_IPC_MSG_CPU_OFF, target_state);

	/* trigger IPC message to MSS */
	mss_pm_ipc_msg_trigger();

	/* pm system synchronization */
	bakery_lock_release(&pm_sys_lock);

	/* trace message */
	PM_TRACE(TRACE_PWR_DOMAIN_OFF, idx);
#else
	INFO("a8k_pwr_domain_off is not supported without SCP\n");
	return;
#endif /* SCP_IMAGE */
}

/*******************************************************************************
 * A8K handler called when a power domain is about to be suspended. The
 * target_state encodes the power state that each level should transition to.
 ******************************************************************************/
void a8k_pwr_domain_suspend(const psci_power_state_t *target_state)
{
#ifdef SCP_IMAGE
	unsigned int idx;

	/* Prevent interrupts from spuriously waking up this cpu */
	gicv2_cpuif_disable();

	idx = plat_my_core_pos();

	/* pm system synchronization -used to synchronize multiple core access to MSS */
	bakery_lock_get(&pm_sys_lock);

	/* send CPU Suspend IPC Message to MSS */
	mss_pm_ipc_msg_send(idx, PM_IPC_MSG_CPU_SUSPEND, target_state);

	/* trigger IPC message to MSS */
	mss_pm_ipc_msg_trigger();

	/* pm system synchronization */
	bakery_lock_release(&pm_sys_lock);

	/* trace message */
	PM_TRACE(TRACE_PWR_DOMAIN_SUSPEND, idx);
#else
	INFO("a8k_pwr_domain_suspend is not supported without SCP\n");
	return;
#endif /* SCP_IMAGE */
}

/*******************************************************************************
 * A8K handler called when a power domain has just been powered on after
 * being turned off earlier. The target_state encodes the low power state that
 * each level has woken up from.
 ******************************************************************************/
void a8k_pwr_domain_on_finish(const psci_power_state_t *target_state)
{
	/* arch specific configuration */
	psci_arch_init();

	/* Interrupt initialization */
	gicv2_pcpu_distif_init();
	gicv2_cpuif_enable();

#ifdef SCP_IMAGE
	/* trace message */
	PM_TRACE(TRACE_PWR_DOMAIN_ON_FINISH, idx);
#endif /* SCP_IMAGE */
}

/*******************************************************************************
 * A8K handler called when a power domain has just been powered on after
 * having been suspended earlier. The target_state encodes the low power state
 * that each level has woken up from.
 * TODO: At the moment we reuse the on finisher and reinitialize the secure
 * context. Need to implement a separate suspend finisher.
 ******************************************************************************/
void a8k_pwr_domain_suspend_finish(const psci_power_state_t *target_state)
{
#ifdef SCP_IMAGE
	/* arch specific configuration */
	psci_arch_init();

	/* Interrupt initialization */
	gicv2_cpuif_enable();

	/* trace message */
	PM_TRACE(TRACE_PWR_DOMAIN_SUSPEND_FINISH, idx);
#else
	INFO("a8k_pwr_domain_on_finish is not supported without SCP\n");
	return;
#endif /* SCP_IMAGE */
}

/*******************************************************************************
 * A8K handlers to shutdown/reboot the system
 ******************************************************************************/
static void __dead2 a8k_system_off(void)
{
	ERROR("a8k_system_off needs to be implemented\n");
	panic();
	wfi();
	ERROR("A8K System Off: operation not handled.\n");
	panic();
}

static void __dead2 a8k_system_reset(void)
{
	plat_marvell_system_reset();

	/* we shouldn't get to this point */
	panic();
}

/*******************************************************************************
 * Export the platform handlers via plat_arm_psci_pm_ops. The ARM Standard
 * platform layer will take care of registering the handlers with PSCI.
 ******************************************************************************/
const plat_psci_ops_t plat_arm_psci_pm_ops = {
	.cpu_standby = a8k_cpu_standby,
	.pwr_domain_on = a8k_pwr_domain_on,
	.pwr_domain_off = a8k_pwr_domain_off,
	.pwr_domain_suspend = a8k_pwr_domain_suspend,
	.pwr_domain_on_finish = a8k_pwr_domain_on_finish,
	.pwr_domain_suspend_finish = a8k_pwr_domain_suspend_finish,
	.system_off = a8k_system_off,
	.system_reset = a8k_system_reset,
	.validate_power_state = a8k_validate_power_state,
	.validate_ns_entrypoint = a8k_validate_ns_entrypoint
};