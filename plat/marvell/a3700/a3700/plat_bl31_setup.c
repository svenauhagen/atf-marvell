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
#include <plat_private.h>
#include <sys_info.h>
#include <mmio.h>
#include <a3700_dram_cs.h>
#include <io_addr_dec.h>
#include <plat_config.h>
#include <dram_win.h>

/* This function passes DRAM size in ATF to sys info */
static void pass_dram_sys_info(void)
{
	uint32_t cs_id, base_low, base_high, size_mbytes;

	for (cs_id = 0; cs_id < MVEBU_MAX_CS_MMAP_NUM; cs_id++) {
		if (marvell_get_dram_cs_base_size(cs_id, &base_low, &base_high, &size_mbytes)) {
			set_info(DRAM_CS0 + cs_id, 0);
		} else {
			set_info(DRAM_CS0 + cs_id, 1);
			/* Pass DRAM size value, so that u-boot could get it later */
			set_info(DRAM_CS0_SIZE + cs_id, size_mbytes);
		}
	}
}

/* This routine does MPP initialization */
static void marvell_bl31_mpp_init(void)
{
	mmio_clrbits_32(MVEBU_NB_GPIO_SEL_REG, 1 << MVEBU_GPIO_TW1_GPIO_EN_OFF);

	/* Set hiden GPIO setting for SPI.
	 * In north_bridge_pin_out_en_high register 13804,
	 * bit 28 is the one which enables CS, CLK pins to be
	 * output, need to set it to 1.
	 * The initial value of this bit is 1, but in UART boot mode
	 * initialization, this bit is disabled and the SPI CS and CLK pins
	 * are used for downloading image purpose; so after downloading,
	 * we should set this bit to 1 again to enable SPI CS and CLK pins.
	 * And anyway, this bit value sould be 1 in all modes,
	 * so here we does not judge boot mode and set this bit to 1 always.
	 */
	mmio_setbits_32(MVEBU_NB_GPIO_OUTPUT_EN_HIGH_REG, 1 << MVEBU_GPIO_NB_SPI_PIN_MODE_OFF);
}

/* This function overruns the same function in marvell_bl31_setup.c */
void bl31_plat_arch_setup(void)
{
	struct dec_win_config *io_dec_map;
	uint32_t dec_win_num;
	struct dram_win_map dram_wins_map;

	marvell_bl31_plat_arch_setup();

	/* MPP init */
	marvell_bl31_mpp_init();

	/* initiliaze the timer for delay functionality */
	plat_delay_timer_init();

	/* Pass DRAM size value so that u-boot could get it later */
	pass_dram_sys_info();

	/* fetch CPU-DRAM window mapping information by reading
	 * CPU-DRAM decode windows (only the enabled ones)
	 */
	dram_win_map_build(&dram_wins_map);

	/* Get IO address decoder windows */
	if (marvell_get_io_dec_win_conf(&io_dec_map, &dec_win_num)) {
		printf("No IO address decoder windows configurations found!\n");
		return;
	}

	/* IO address decoder init */
	if (init_io_addr_dec(&dram_wins_map, io_dec_map, dec_win_num)) {
		printf("IO address decoder windows initialization failed!\n");
		return;
	}
}
