/************************************************************************************
 * arch/arm/src/tiva/cc13xx/cc13x0_rom.c
 *
 *   Copyright (C) 2019 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * This is a port of TI's setup_rom.c file which has a fully compatible BSD license:
 *
 *    Copyright (c) 2015-2017, Texas Instruments Incorporated
 *    All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ************************************************************************************/

/************************************************************************************
 * Included Files
 ************************************************************************************/

#include <stdint.h>

#include "up_arch.h"

#include "hardware/tiva_adi2_refsys.h"
#include "hardware/tiva_adi3_refsys.h"
#include "hardware/tiva_adi4_aux.h"
#include "hardware/tiva_aon_batmon.h"
#include "hardware/tiva_aon_rtc.h"
#include "hardware/tiva_aon_sysctl.h"
#include "hardware/tiva_aux_wuc.h"
#include "hardware/tiva_ccfg.h"
#include "hardware/tiva_ddi0_osc.h"
#include "hardware/tiva_fcfg1.h"
#include "hardware/tiva_vims.h"

#include "cc13xx/cc13x0_rom.h"

/************************************************************************************
 * Private Functions
 ************************************************************************************/

/************************************************************************************
 * Name: rom_set_vddr_level
 ************************************************************************************/

static void rom_set_vddr_level(uint32_t ccfg_modeconf)
{
  uint32_t rawtrim;
  int32_t targettrim;
  int32_t currenttrim;
  int32_t deltatrim;
  uint16_t regval16;

  /* VDDS_BOD_LEVEL = 1 means that boost mode is selected - Step up VDDR_TRIM
   * to FCFG1..VDDR_TRIM_HH */

  rawtrim     = ((getreg32(TIVA_FCFG1_VOLT_TRIM) &
                  FCFG1_VOLT_TRIM_VDDR_TRIM_HH_MASK) >>
                 FCFG1_VOLT_TRIM_VDDR_TRIM_HH_SHIFT);

  targettrim  = rom_signextend_vddrtrim(rawtrim);
  currenttrim =
    rom_signextend_vddrtrim((getreg8(TIVA_ADI3_REFSYS_DCDCCTL0) &
                             ADI3_REFSYS_DCDCCTL0_VDDR_TRIM_MASK) >>
                            ADI3_REFSYS_DCDCCTL0_VDDR_TRIM_SHIFT);

  if (currenttrim != targettrim)
    {
      /* Disable VDDR BOD */

      modifyreg32(TIVA_AON_SYSCTL_RESETCTL,
                  AON_SYSCTL_RESETCTL_VDDR_LOSS_EN, 0);

      while (currenttrim != targettrim)
        {
          deltatrim = targettrim - currenttrim;
          if (deltatrim > 2)
            {
              deltatrim = 2;
            }

          if (deltatrim < -2)
            {
              deltatrim = -2;
            }

          currenttrim += deltatrim;

          (void)getreg32(TIVA_AON_RTC_SYNC); /* Wait one SCLK_LF period */

          regval16 = (ADI3_REFSYS_DCDCCTL0_VDDR_TRIM_MASK << 8) |
                     ((currenttrim << ADI3_REFSYS_DCDCCTL0_VDDR_TRIM_SHIFT) &
                      ADI3_REFSYS_DCDCCTL0_VDDR_TRIM_MASK);
          putreg16(regval16,
                   TIVA_ADI3_REFSYS_MASK8B + (TIVA_ADI3_REFSYS_DCDCCTL0_OFFSET * 2));

          /* Force SCLK_LF period wait on next read */

          putreg32(1, TIVA_AON_RTC_SYNC);
        }

      (void)getreg32(TIVA_AON_RTC_SYNC);     /* Wait one SCLK_LF period */

      /* Force SCLK_LF period wait on  next read */

      putreg32(1, TIVA_AON_RTC_SYNC);

      (void)getreg32(TIVA_AON_RTC_SYNC);     /* Wait one more SCLK_LF period
                                                 * before re-enabling VDDR BOD */

      modifyreg32(TIVA_AON_SYSCTL_RESETCTL, 0,
                 AON_SYSCTL_RESETCTL_VDDR_LOSS_EN);
      (void)getreg32(TIVA_AON_RTC_SYNC);     /* And finally wait for
                                                 * VDDR_LOSS_EN setting to
                                                 * propagate */
    }
}

/************************************************************************************
 * Public Functions
 ************************************************************************************/

/************************************************************************************
 * Name: rom_setup_coldreset_from_shutdown_cfg1
 ************************************************************************************/

void rom_setup_coldreset_from_shutdown_cfg1(uint32_t ccfg_modeconf)
{
  int32_t vddr_sleeptrim;
  int32_t vddr_sleepdelta;
  uint32_t setbits;
  uint32_t clrbits;
  uint16_t regval16;

  /* Check for CC13xx boost mode The combination VDDR_EXT_LOAD=0 and
   * VDDS_BOD_LEVEL=1 is defined to select boost mode */

  if (((ccfg_modeconf & CCFG_MODE_CONF_VDDR_EXT_LOAD) == 0) &&
      ((ccfg_modeconf & CCFG_MODE_CONF_VDDS_BOD_LEVEL) != 0))
    {
      /* Set VDDS_BOD trim - using masked write {MASK8:DATA8} - TRIM_VDDS_BOD
       * is bits[7:3] of ADI3..REFSYSCTL1 - Needs a positive transition on
       * BOD_BG_TRIM_EN (bit[7] of REFSYSCTL3) to latch new VDDS BOD. Set to 0
       * first to guarantee a positive transition. */

      putreg8(ADI3_REFSYS_REFSYSCTL3_BOD_BG_TRIM_EN,
              TIVA_ADI3_REFSYS_CLR + TIVA_ADI3_REFSYS_REFSYSCTL3_OFFSET);

      /* VDDS_BOD_LEVEL = 1 means that boost mode is selected
       * -Max out the VDDS_BOD trim( = VDDS_BOD_POS_31)
       */

      putreg16((ADI3_REFSYS_REFSYSCTL1_TRIM_VDDS_BOD_MASK << 8) |
               (ADI3_REFSYS_REFSYSCTL1_TRIM_VDDS_BOD_POS_31),
               TIVA_ADI3_REFSYS_MASK8B + (TIVA_ADI3_REFSYS_REFSYSCTL1_OFFSET * 2));
      putreg8(ADI3_REFSYS_REFSYSCTL3_BOD_BG_TRIM_EN,
              TIVA_ADI3_REFSYS_SET + TIVA_ADI3_REFSYS_REFSYSCTL3_OFFSET);

      rom_set_vddr_level(ccfg_modeconf);

      vddr_sleeptrim =
        rom_signextend_vddrtrim((getreg32(TIVA_FCFG1_VOLT_TRIM) &
                                      FCFG1_VOLT_TRIM_VDDR_TRIM_SLEEP_H_MASK) >>
                                     FCFG1_VOLT_TRIM_VDDR_TRIM_SLEEP_H_SHIFT);
    }
  else
    {
      vddr_sleeptrim =
        rom_signextend_vddrtrim((getreg32(TIVA_FCFG1_LDO_TRIM) &
                                      FCFG1_LDO_TRIM_VDDR_TRIM_SLEEP_MASK) >>
                                     FCFG1_LDO_TRIM_VDDR_TRIM_SLEEP_SHIFT);
    }

  /* Adjust the VDDR_TRIM_SLEEP value with value adjustable by customer
   * (CCFG_MODE_CONF_VDDR_TRIM_SLEEP_DELTA) Read and sign extend VddrSleepDelta
   * (in range -8 to +7) */

  vddr_sleepdelta =
    (((int32_t)
      (ccfg_modeconf <<
       (32 - CCFG_MODE_CONF_VDDR_TRIM_SLEEP_DELTA_WIDTH -
        CCFG_MODE_CONF_VDDR_TRIM_SLEEP_DELTA_SHIFT))) >> (32 -
                                                      CCFG_MODE_CONF_VDDR_TRIM_SLEEP_DELTA_WIDTH));
  /* Calculate new VDDR sleep trim */

  vddr_sleeptrim = (vddr_sleeptrim + vddr_sleepdelta + 1);
  if (vddr_sleeptrim > 21)
    {
      vddr_sleeptrim = 21;
    }

  if (vddr_sleeptrim < -10)
    {
      vddr_sleeptrim = -10;
    }

  /* Write adjusted value using MASKED write (MASK8) */

  regval16 = ((ADI3_REFSYS_DCDCCTL1_VDDR_TRIM_SLEEP_MASK << 8) |
              ((vddr_sleeptrim << ADI3_REFSYS_DCDCCTL1_VDDR_TRIM_SLEEP_SHIFT) &
               ADI3_REFSYS_DCDCCTL1_VDDR_TRIM_SLEEP_MASK));
  putreg16(regval16,
           TIVA_ADI3_REFSYS_MASK8B + (TIVA_ADI3_REFSYS_DCDCCTL1_OFFSET * 2));

  /* 1. Do not allow DCDC to be enabled if in external regulator mode.
   * Preventing this by setting both the RECHARGE and the ACTIVE bits bit in
   * the CCFG_MODE_CONF copy register (ccfg_modeconf). 2. Adjusted battery
   * monitor low limit in internal regulator mode. This is done by setting
   * AON_BATMON_FLASHPUMPP0_LOWLIM=0 in internal regulator mode.
   */

  if (getreg32(TIVA_AON_SYSCTL_PWRCTL) &
      AON_SYSCTL_PWRCTL_EXT_REG_MODE)
    {
      ccfg_modeconf |= (CCFG_MODE_CONF_DCDC_RECHARGE |
                        CCFG_MODE_CONF_DCDC_ACTIVE);
    }
  else
    {
      modifyreg32(TIVA_AON_BATMON_FLASHPUMPP0,
                  AON_BATMON_FLASHPUMPP0_LOWLIM_BITN, 0);
    }

  /* Set the RECHARGE source based upon CCFG:MODE_CONF:DCDC_RECHARGE Note:
   * Inverse polarity
   */

  setbits = 0;
  clrbits = 0;

  if ((ccfg_modeconf & CCFG_MODE_CONF_DCDC_RECHARGE) != 0)
    {
      clrbits |= AON_SYSCTL_PWRCTL_DCDC_EN;
    }
  else
    {
      setbits |= AON_SYSCTL_PWRCTL_DCDC_EN;
    }

  /* Set the ACTIVE source based upon CCFG:MODE_CONF:DCDC_ACTIVE Note: Inverse
   * polarity
   */

  if ((ccfg_modeconf & CCFG_MODE_CONF_DCDC_ACTIVE) != 0)
    {
      clrbits |= AON_SYSCTL_PWRCTL_DCDC_ACTIVE;
    }
  else
    {
      setbits |= AON_SYSCTL_PWRCTL_DCDC_ACTIVE;
    }

  modifyreg32(TIVA_AON_SYSCTL_PWRCTL, clrbits, setbits);
}

/************************************************************************************
 * Name: rom_setup_coldreset_from_shutdown_cfg2
 ************************************************************************************/

void rom_setup_coldreset_from_shutdown_cfg2(uint32_t fcfg1_revision,
                                            uint32_t ccfg_modeconf)
{
  uint32_t trim;

  /* Following sequence is required for using XOSCHF, if not included devices
   * crashes when trying to switch to XOSCHF. Trim CAP settings. Get and set
   * trim value for the ANABYPASS_VALUE1 register */

  trim = rom_setup_get_trim_anabypass_value1(ccfg_modeconf);
  rom_ddi_write32(TIVA_AUX_DDI0_OSC_BASE, TIVA_DDI0_OSC_ANABYPASSVAL1_OFFSET, trim);

  /* Trim RCOSC_LF. Get and set trim values for the RCOSCLF_RTUNE_TRIM and
   * RCOSCLF_CTUNE_TRIM fields in the XOSCLF_RCOSCLF_CTRL register. */

  trim = rom_setup_get_trim_rcosc_lfrtunectuntrim();
  rom_ddi_bitfield_write16(TIVA_AUX_DDI0_OSC_BASE, TIVA_DDI0_OSC_LFOSCCTL_OFFSET,
                     (DDI0_OSC_LFOSCCTL_RCOSCLF_CTUNE_TRIM |
                      DDI0_OSC_LFOSCCTL_RCOSCLF_RTUNE_TRIM),
                     DDI0_OSC_LFOSCCTL_RCOSCLF_CTUNE_TRIM_SHIFT, trim);

  /* Trim XOSCHF IBIAS THERM. Get and set trim value for the XOSCHF IBIAS THERM
   * bit field in the ANABYPASS_VALUE2 register. Other register bit fields are
   * set to 0. */

  trim = rom_setup_get_trim_xosc_hfibiastherm();
  rom_ddi_write32(TIVA_AUX_DDI0_OSC_BASE, TIVA_DDI0_OSC_ANABYPASSVAL2_OFFSET,
                trim << DDI0_OSC_ANABYPASSVAL2_XOSC_HF_IBIASTHERM_SHIFT);

  /* Trim AMPCOMP settings required before switch to XOSCHF */

  trim = rom_setup_get_trim_ampcompth2();
  rom_ddi_write32(TIVA_AUX_DDI0_OSC_BASE, TIVA_DDI0_OSC_AMPCOMPTH2_OFFSET, trim);
  trim = rom_setup_get_trim_ampcompth1();
  rom_ddi_write32(TIVA_AUX_DDI0_OSC_BASE, TIVA_DDI0_OSC_AMPCOMPTH1_OFFSET, trim);
  trim = rom_setup_get_trim_ampcompctrl(fcfg1_revision);
  rom_ddi_write32(TIVA_AUX_DDI0_OSC_BASE, TIVA_DDI0_OSC_AMPCOMPCTL_OFFSET, trim);

  /* Set trim for DDI0_OSC_ADCDOUBLERNANOAMPCTL_ADC_SH_MODE_EN in accordance
   * to FCFG1 setting This is bit[5] in the TIVA_DDI0_OSC_ADCDOUBLERNANOAMPCTL
   * register Using MASK4 write + 1 => writing to bits[7:4] */

  trim = rom_setup_get_trim_adcshmodeen(fcfg1_revision);
  putreg8((0x20 | (trim << 1),
          TIVA_AUX_DDI0_OSC_BASE + TIVA_DDI_MASK4B_OFFSET +
          (TIVA_DDI0_OSC_ADCDOUBLERNANOAMPCTL_OFFSET * 2) + 1));

  /* Set trim for DDI0_OSC_ADCDOUBLERNANOAMPCTL_ADC_SH_VBUF_EN in accordance
   * to FCFG1 setting This is bit[4] in the TIVA_DDI0_OSC_ADCDOUBLERNANOAMPCTL
   * register Using MASK4 write + 1 => writing to bits[7:4] */

  trim = rom_setup_get_trim_adcshvbufen(fcfg1_revision);
  putreg8(0x10 | trim,
          TIVA_AUX_DDI0_OSC_BASE + TIVA_DI_MASK4B_OFFSET +
          (TIVA_DDI0_OSC_ADCDOUBLERNANOAMPCTL_OFFSET * 2) + 1);

  /* Set trim for the PEAK_DET_ITRIM, HP_BUF_ITRIM and LP_BUF_ITRIM bit fields
   * in the TIVA_DDI0_OSC_XOSCHFCTL register in accordance to FCFG1 setting.
   * Remaining register bit fields are set to their reset values of 0. */

  trim = rom_setup_get_trim_xosc_hfctrl(fcfg1_revision);
  rom_ddi_write32(TIVA_AUX_DDI0_OSC_BASE, TIVA_DDI0_OSC_XOSCHFCTL_OFFSET, trim);

  /* Set trim for DBLR_LOOP_FILTER_RESET_VOLTAGE in accordance to FCFG1 setting
   * (This is bits [18:17] in TIVA_DDI0_OSC_ADCDOUBLERNANOAMPCTL) (Using MASK4
   * write + 4 => writing to bits[19:16] => (4*4)) (Assuming:
   * DDI0_OSC_ADCDOUBLERNANOAMPCTL_DBLR_LOOP_FILTER_RESET_VOLTAGE_SHIFT = 17 and
   * that DDI0_OSC_ADCDOUBLERNANOAMPCTL_DBLR_LOOP_FILTER_RESET_VOLTAGE_MASK =
   * 0x00060000)
   */

  trim = rom_setup_get_trim_dblrloopfilter_resetvoltage(fcfg1_revision);
  putreg8(0x60 | (trim << 1),
          TIVA_AUX_DDI0_OSC_BASE + TIVA_DDI_MASK4B_OFFSET +
          (TIVA_DDI0_OSC_ADCDOUBLERNANOAMPCTL_OFFSET * 2) + 4);

  /* Update DDI0_OSC_ATESTCTL_ATESTLF_RCOSCLF_IBIAS_TRIM with data from
   * FCFG1_OSC_CONF_ATESTLF_RCOSCLF_IBIAS_TRIM This is TIVA_DDI0_OSC_ATESTCTL
   * bit[7] ( TIVA_DDI0_OSC_ATESTCTL is currently hidden (but=0x00000020)) Using
   * MASK4 write + 1 => writing to bits[7:4]
   */

  trim = rom_setup_get_trim_rcosc_lfibiastrim(fcfg1_revision);
  putreg8(0x80 | (trim << 3),
          TIVA_AUX_DDI0_OSC_BASE + TIVA_DDI_MASK4B_OFFSET +
          (0x00000020 * 2) + 1);

  /* Update DDI0_OSC_LFOSCCTL_XOSCLF_REGULATOR_TRIM and
   * DDI0_OSC_LFOSCCTL_XOSCLF_CMIRRWR_RATIO in one write This can be
   * simplified since the registers are packed together in the same order both
   * in FCFG1 and in the HW register. This spans TIVA_DDI0_OSC_LFOSCCTL
   * bits[23:18] Using MASK8 write + 4 => writing to bits[23:16]
   */

  trim = rom_setup_get_trim_lfregulator_cmirrwr_ratio(fcfg1_revision);
  putreg16(0xfc00 | (trim << 2),
           TIVA_AUX_DDI0_OSC_BASE + TIVA_DDI_MASK8B_OFFSET +
           (TIVA_DDI0_OSC_LFOSCCTL_OFFSET * 2) + 4);

  /* Set trim the HPM_IBIAS_WAIT_CNT, LPM_IBIAS_WAIT_CNT and IDAC_STEP bit
   * fields in the TIVA_DDI0_OSC_RADCEXTCFG register in accordance to FCFG1
   * setting. Remaining register bit fields are set to their reset values of 0. */

  trim = rom_setup_get_trim_radc_extcfg(fcfg1_revision);
  rom_ddi_write32(TIVA_AUX_DDI0_OSC_BASE, TIVA_DDI0OSC_RADCEXTCFG_OFFSET, trim);

  /* Setting FORCE_KICKSTART_EN (ref. CC26_V1_BUG00261). Should also be done
   * for PG2 (This is bit 22 in TIVA_DDI0_OSC_CTL0) */

  putreg32(DDI0_OSC_CTL0_FORCE_KICKSTART_EN,
           TIVA_AUX_DDI0_OSC_BASE + TIVA_DDI_SET_OFFSET +
           TIVA_DDI0_OSC_CTL0_OFFSET);
}

/************************************************************************************
 * Name: rom_setup_coldreset_from_shutdown_cfg13
 ************************************************************************************/

void rom_setup_coldreset_from_shutdown_cfg3(uint32_t ccfg_modeconf)
{
  uint32_t fcfg1_oscconf;
  uint32_t trim;
  uint32_t current_hfclock;
  uint32_t ccfg_extlfclk;
  uint32_t regval;
  uint8_t regval8;

  /* Examine the XOSC_FREQ field to select 0x1=HPOSC, 0x2=48MHz XOSC, 0x3=24MHz
   * XOSC */

  switch ((ccfg_modeconf & CCFG_MODE_CONF_XOSC_FREQ_MASK) >>
          CCFG_MODE_CONF_XOSC_FREQ_SHIFT)
    {
    case 2:
      /* XOSC source is a 48 MHz crystal Do nothing (since this is the reset
       * setting)
       */

      break;

    case 1:
      /* XOSC source is HPOSC (trim the HPOSC if this is a chip with HPOSC,
       * otherwise skip trimming and default to 24 MHz XOSC)
       */

      fcfg1_oscconf = getreg32(TIVA_FCFG1_OSC_CONF);

      if ((fcfg1_oscconf & FCFG1_OSC_CONF_HPOSC_OPTION) == 0)
        {
          /* This is a HPOSC chip, apply HPOSC settings Set bit
           * DDI0_OSC_CTL0_HPOSC_MODE_EN (this is bit 14 in TIVA_DDI0_OSC_CTL0) */

          putreg32(DDI0_OSC_CTL0_HPOSC_MODE_EN,
                   TIVA_AUX_DDI0_OSC_BASE + TIVA_DDI_SET_OFFSET +
                   TIVA_DDI_OSC_CTL0_OFFSET);

          /* ADI2_REFSYS_HPOSCCTL2_BIAS_HOLD_MODE_EN =
           * FCFG1_OSC_CONF_HPOSC_BIAS_HOLD_MODE_EN (1 bit)
           * ADI2_REFSYS_HPOSCCTL2_CURRMIRR_RATIO =
           * FCFG1_OSC_CONF_HPOSC_CURRMIRR_RATIO (4 bits)
           * ADI2_REFSYS_HPOSCCTL1_BIAS_RES_SET =
           * FCFG1_OSC_CONF_HPOSC_BIAS_RES_SET (4 bits)
           * ADI2_REFSYS_HPOSCCTL0_FILTER_EN = FCFG1_OSC_CONF_HPOSC_FILTER_EN
           * (1 bit) ADI2_REFSYS_HPOSCCTL0_BIAS_RECHARGE_DLY =
           * FCFG1_OSC_CONF_HPOSC_BIAS_RECHARGE_DELAY (2 bits)
           * ADI2_REFSYS_HPOSCCTL0_SERIES_CAP =
           * FCFG1_OSC_CONF_HPOSC_SERIES_CAP (2 bits)
           * ADI2_REFSYS_HPOSCCTL0_DIV3_BYPASS =
           * FCFG1_OSC_CONF_HPOSC_DIV3_BYPASS (1 bit) */

          regval = ((getreg32(TIVA_ADI2_REFSYS_HPOSCCTL2) &
                     ~(ADI2_REFSYS_HPOSCCTL2_BIAS_HOLD_MODE_EN |
                       ADI2_REFSYS_HPOSCCTL2_CURRMIRR_RATIO_MASK)) |
                    (((fcfg1_oscconf &
                       FCFG1_OSC_CONF_HPOSC_BIAS_HOLD_MODE_EN_MASK) >>
                      FCFG1_OSC_CONF_HPOSC_BIAS_HOLD_MODE_EN_SHIFT) <<
                     ADI2_REFSYS_HPOSCCTL2_BIAS_HOLD_MODE_EN_SHIFT) |
                    (((fcfg1_oscconf &
                       FCFG1_OSC_CONF_HPOSC_CURRMIRR_RATIO_MASK) >>
                      FCFG1_OSC_CONF_HPOSC_CURRMIRR_RATIO_SHIFT) <<
                     ADI2_REFSYS_HPOSCCTL2_CURRMIRR_RATIO_SHIFT));
          putreg32(regval, TIVA_ADI2_REFSYS_HPOSCCTL2);

          regval = ((getreg32(TIVA_ADI2_REFSYS_HPOSCCTL1) &
                     ~(ADI2_REFSYS_HPOSCCTL1_BIAS_RES_SET_MASK)) |
                    (((fcfg1_oscconf &
                       FCFG1_OSC_CONF_HPOSC_BIAS_RES_SET_MASK) >>
                      FCFG1_OSC_CONF_HPOSC_BIAS_RES_SET_SHIFT) <<
                     ADI2_REFSYS_HPOSCCTL1_BIAS_RES_SET_SHIFT));
          putreg32(regval, TIVA_ADI2_REFSYS_HPOSCCTL1);

          regval = ((getreg32(TIVA_ADI2_REFSYS_HPOSCCTL0) &
                     ~(ADI2_REFSYS_HPOSCCTL0_FILTER_EN_MASK |
                       ADI2_REFSYS_HPOSCCTL0_BIAS_RECHARGE_DLY_MASK |
                       ADI2_REFSYS_HPOSCCTL0_SERIES_CAP_MASK |
                       ADI2_REFSYS_HPOSCCTL0_DIV3_BYPASS_MASK)) |
                     (((fcfg1_oscconf &
                        FCFG1_OSC_CONF_HPOSC_FILTER_EN_MASK) >>
                       FCFG1_OSC_CONF_HPOSC_FILTER_EN_SHIFT) <<
                      ADI2_REFSYS_HPOSCCTL0_FILTER_EN_SHIFT) |
                     (((fcfg1_oscconf &
                        FCFG1_OSC_CONF_HPOSC_BIAS_RECHARGE_DELAY_MASK) >>
                       FCFG1_OSC_CONF_HPOSC_BIAS_RECHARGE_DELAY_SHIFT) <<
                      ADI2_REFSYS_HPOSCCTL0_BIAS_RECHARGE_DLY_SHIFT) |
                     (((fcfg1_oscconf &
                        FCFG1_OSC_CONF_HPOSC_SERIES_CAP_MASK) >>
                       FCFG1_OSC_CONF_HPOSC_SERIES_CAP_SHIFT) <<
                      ADI2_REFSYS_HPOSCCTL0_SERIES_CAP_SHIFT) |
                     (((fcfg1_oscconf &
                        FCFG1_OSC_CONF_HPOSC_DIV3_BYPASS_MASK) >>
                       FCFG1_OSC_CONF_HPOSC_DIV3_BYPASS_SHIFT) <<
                      ADI2_REFSYS_HPOSCCTL0_DIV3_BYPASS_SHIFT));
          putreg32(regval, TIVA_ADI2_REFSYS_HPOSCCTL0);
          break;
        }
      /* Not a HPOSC chip - fall through to default */

    default:
      /* XOSC source is a 24 MHz crystal (default) Set bit
       * DDI0_OSC_CTL0_XTAL_IS_24M (this is bit 31 in TIVA_DDI0_OSC_CTL0) */

      putreg32(DDI0_OSC_CTL0_XTAL_IS_24M,
               TIVA_AUX_DDI0_OSC_BASE + TIVA_DDI_SET_OFFSET +
               TIVA_DDI0_OSC_CTL0_OFFSET);
      break;
    }

  /* Set XOSC_HF in bypass mode if CCFG is configured for external TCXO Please
   * note that it is up to the customer to make sure that the external clock
   * source is up and running before XOSC_HF can be used. */

  if ((getreg32(TIVA_CCFG_SIZE_AND_DIS_FLAGS) &
       CCFG_SIZE_AND_DIS_FLAGS_DIS_TCXO) == 0)
    {
      putreg32(DDI0_OSC_XOSCHFCTL_BYPASS,
               TIVA_AUX_DDI0_OSC_BASE + TIVA_DDI_SET_OFFSET + TIVA_DDI0_OSC_XOSCHFCTL_OFFSET);
    }

  /* Clear DDI0_OSC_CTL0_CLK_LOSS_EN (ClockLossEventEnable()). This is bit 9
   * in TIVA_DDI0_OSC_CTL0. This is typically already 0 except on Lizard where it
   * is set in ROM-boot */

  putreg32(DDI0_OSC_CTL0_CLK_LOSS_EN
           TIVA_AUX_DDI0_OSC_BASE + TIVA_DDI_CLR_OFFSET +
           TIVA_DDI0_OSC_CTL0_OFFSET);

  /* Setting DDI0_OSC_CTL1_XOSC_HF_FAST_START according to value found in
   * FCFG1
   */

  trim = rom_setup_get_trim_xosc_hffaststart();
  putreg8((0x30 | trim),
          TIVA_AUX_DDI0_OSC_BASE + TIVA_DDI_MASK4B_OFFSET +
          (TIVA_DDI0_OSC_CTL1_OFFSET * 2));

  /* setup the LF clock based upon CCFG:MODE_CONF:SCLK_LF_OPTION */

  switch ((ccfg_modeconf & CCFG_MODE_CONF_SCLK_LF_OPTION_MASK) >>
          CCFG_MODE_CONF_SCLK_LF_OPTION_SHIFT)
    {
    case 0:                    /* XOSC_HF_DLF (XOSCHF/1536) -> SCLK_LF (=31250
                                 * Hz) */

      rom_osc_set_clocksource(OSC_SRC_CLK_LF, OSC_XOSC_HF);
      rom_setup_aonrtc_subsecinc(0x8637bd);        /* RTC_INCREMENT = 2^38 /
                                                 * frequency */

      break;
    case 1:                    /* EXTERNAL signal -> SCLK_LF
                                 * (frequency=2^38/CCFG_EXT_LF_CLK_RTC_INCREMENT)
                                 * * Set SCLK_LF to use the same source as SCLK_HF
                                 * * Can be simplified a bit since possible return
                                 * values for HF matches LF settings */

      current_hfclock = rom_osc_get_clocksource(OSC_SRC_CLK_HF);
      rom_osc_set_clocksource(OSC_SRC_CLK_LF, current_hfclock);
      while (rom_osc_get_clocksource(OSC_SRC_CLK_LF) != current_hfclock)
        {
          /* Wait until switched */

        }
      ccfg_extlfclk = getreg32(TIVA_CCFG_EXT_LF_CLK);
      rom_setup_aonrtc_subsecinc((ccfg_extlfclk & CCFG_EXT_LF_CLK_RTC_INCREMENT_MASK)
                              >> CCFG_EXT_LF_CLK_RTC_INCREMENT_SHIFT);

       /* Route external clock to AON IOC w/hysteresis.  Set XOSC_LF in
        * bypass mode to allow external 32 kHz clock
        */

      rom_iocport_set_configuration((ccfg_extlfclk & CCFG_EXT_LF_CLK_DIO_MASK) >> CCFG_EXT_LF_CLK_DIO_SHIFT, IOC_PORT_AON_CLK32K, IOC_STD_INPUT | IOC_HYST_ENABLE);

      putreg32(DDI0_OSC_CTL0_XOSC_LF_DIG_BYPASS,
               TIVA_AUX_DDI0_OSC_BASE + TIVA_DDI_SET_OFFSET +
               TIVA_DDI0_OSC_CTL0_OFFSET);

      /* Fall through to set XOSC_LF as SCLK_LF source */

    case 2:                    /* XOSC_LF -> SLCK_LF (32768 Hz) */

      rom_osc_set_clocksource(OSC_SRC_CLK_LF, OSC_XOSC_LF);
      break;
    default:                   /* (=3) RCOSC_LF */

      rom_osc_set_clocksource(OSC_SRC_CLK_LF, OSC_RCOSC_LF);
      break;
    }

  /* Update ADI_4_AUX_ADCREF1_VTRIM with value from FCFG1 */

  regval8 = (((getreg32(TIVA_FCFG1_SOC_ADC_REF_TRIM_AND_OFFSET_EXT) >>
               FCFG1_SOC_ADC_REF_TRIM_AND_OFFSET_EXT_SOC_ADC_REF_VOLTAGE_TRIM_TEMP1_SHIFT)
             << ADI_4_AUX_ADCREF1_VTRIM_SHIFT) & ADI_4_AUX_ADCREF1_VTRIM_MASK);
  putreg8(regval8, TIVA_ADI4_AUX_ADCREF1);

  /* Sync with AON */

  SysCtrlAonSync();
}

/************************************************************************************
 * Name: rom_setup_get_trim_anabypass_value1
 ************************************************************************************/

uint32_t rom_setup_get_trim_anabypass_value1(uint32_t ccfg_modeconf)
{
  uint32_t ui32Fcfg1Value;
  uint32_t ui32XoscHfRow;
  uint32_t ui32XoscHfCol;
  uint32_t trimValue;

  /* Use device specific trim values located in factory configuration area for
   * the XOSC_HF_COLUMN_Q12 and XOSC_HF_ROW_Q12 bit fields in the
   * ANABYPASS_VALUE1 register. Value for the other bit fields are set to 0. */

  ui32Fcfg1Value = getreg32(TIVA_FCFG1_CONFIG_OSC_TOP);
  ui32XoscHfRow = ((ui32Fcfg1Value &
                    FCFG1_CONFIG_OSC_TOP_XOSC_HF_ROW_Q12_MASK) >>
                   FCFG1_CONFIG_OSC_TOP_XOSC_HF_ROW_Q12_SHIFT);
  ui32XoscHfCol = ((ui32Fcfg1Value &
                    FCFG1_CONFIG_OSC_TOP_XOSC_HF_COLUMN_Q12_MASK) >>
                   FCFG1_CONFIG_OSC_TOP_XOSC_HF_COLUMN_Q12_SHIFT);

  if ((ccfg_modeconf & CCFG_MODE_CONF_XOSC_CAP_MOD) == 0)
    {
      /* XOSC_CAP_MOD = 0 means: CAP_ARRAY_DELTA is in use -> Apply
       * compensation XOSC_CAPARRAY_DELTA is located in bit[15:8] of
       * ccfg_modeconf Note: HW_REV_DEPENDENT_IMPLEMENTATION. Field width is
       * not given by a define and sign extension must therefore be hard coded.
       * ( A small test program is created verifying the code lines below: Ref.:
       * ..\test\small_standalone_test_programs\CapArrayDeltaAdjust_test.c) */

      int32_t i32CustomerDeltaAdjust =
        (((int32_t)
          (ccfg_modeconf <<
           (32 - CCFG_MODE_CONF_XOSC_CAPARRAY_DELTA_WIDTH -
            CCFG_MODE_CONF_XOSC_CAPARRAY_DELTA_SHIFT))) >> (32 -
                                                        CCFG_MODE_CONF_XOSC_CAPARRAY_DELTA_WIDTH));

      while (i32CustomerDeltaAdjust < 0)
        {
          ui32XoscHfCol >>= 1;  /* COL 1 step down */

          if (ui32XoscHfCol == 0)
            {                   /* if COL below minimum */

              ui32XoscHfCol = 0xffff;   /* Set COL to maximum */

              ui32XoscHfRow >>= 1;      /* ROW 1 step down */

              if (ui32XoscHfRow == 0)
                {               /* if ROW below minimum */

                  ui32XoscHfRow = 1;    /* Set both ROW and COL */

                  ui32XoscHfCol = 1;    /* to minimum */

                }
            }
          i32CustomerDeltaAdjust++;
        }
      while (i32CustomerDeltaAdjust > 0)
        {
          ui32XoscHfCol = (ui32XoscHfCol << 1) | 1;     /* COL 1 step up */

          if (ui32XoscHfCol > 0xffff)
            {                   /* if COL above maximum */

              ui32XoscHfCol = 1;        /* Set COL to minimum */

              ui32XoscHfRow = (ui32XoscHfRow << 1) | 1; /* ROW 1 step up */

              if (ui32XoscHfRow > 0xf)
                {               /* if ROW above maximum */

                  ui32XoscHfRow = 0xf;  /* Set both ROW and COL */

                  ui32XoscHfCol = 0xffff;       /* to maximum */

                }
            }
          i32CustomerDeltaAdjust--;
        }
    }

  trimValue =
    ((ui32XoscHfRow << DDI0_OSC_ANABYPASSVAL1_XOSC_HF_ROW_Q12_SHIFT) |
     (ui32XoscHfCol << DDI0_OSC_ANABYPASSVAL1_XOSC_HF_COLUMN_Q12_SHIFT));

  return (trimValue);
}

/************************************************************************************
 * Name: rom_setup_get_trim_rcosc_lfrtunectuntrim
 ************************************************************************************/

uint32_t rom_setup_get_trim_rcosc_lfrtunectuntrim(void)
{
  uint32_t trimValue;

  /* Use device specific trim values located in factory configuration area */

  trimValue =
    ((getreg32(TIVA_FCFG1_CONFIG_OSC_TOP) &
      FCFG1_CONFIG_OSC_TOP_RCOSCLF_CTUNE_TRIM_MASK) >>
     FCFG1_CONFIG_OSC_TOP_RCOSCLF_CTUNE_TRIM_SHIFT) <<
    DDI0_OSC_LFOSCCTL_RCOSCLF_CTUNE_TRIM_SHIFT;

  trimValue |=
    ((getreg32(TIVA_FCFG1_CONFIG_OSC_TOP) &
      FCFG1_CONFIG_OSC_TOP_RCOSCLF_RTUNE_TRIM_MASK) >>
     FCFG1_CONFIG_OSC_TOP_RCOSCLF_RTUNE_TRIM_SHIFT) <<
    DDI0_OSC_LFOSCCTL_RCOSCLF_RTUNE_TRIM_SHIFT;

  return (trimValue);
}

/************************************************************************************
 * Name: rom_setup_get_trim_xosc_hfibiastherm
 ************************************************************************************/

uint32_t rom_setup_get_trim_xosc_hfibiastherm(void)
{
  uint32_t trimValue;

  /* Use device specific trim value located in factory configuration area */

  trimValue =
    (getreg32(TIVA_FCFG1_ANABYPASS_VALUE2) &
     FCFG1_ANABYPASS_VALUE2_XOSC_HF_IBIASTHERM_MASK) >>
    FCFG1_ANABYPASS_VALUE2_XOSC_HF_IBIASTHERM_SHIFT;

  return (trimValue);
}

/************************************************************************************
 * Name: rom_setup_get_trim_ampcompth2
 ************************************************************************************/

uint32_t rom_setup_get_trim_ampcompth2(void)
{
  uint32_t trimValue;
  uint32_t ui32Fcfg1Value;

  /* Use device specific trim value located in factory configuration area. All
   * defined register bit fields have corresponding trim value in the factory
   * configuration area */

  ui32Fcfg1Value = getreg32(TIVA_FCFG1_AMPCOMP_TH2);
  trimValue = ((ui32Fcfg1Value &
                    FCFG1_AMPCOMP_TH2_LPMUPDATE_LTH_MASK) >>
                   FCFG1_AMPCOMP_TH2_LPMUPDATE_LTH_SHIFT) <<
    DDI0_OSC_AMPCOMPTH2_LPMUPDATE_LTH_SHIFT;
  trimValue |= (((ui32Fcfg1Value &
                      FCFG1_AMPCOMP_TH2_LPMUPDATE_HTM_MASK) >>
                     FCFG1_AMPCOMP_TH2_LPMUPDATE_HTM_SHIFT) <<
                    DDI0_OSC_AMPCOMPTH2_LPMUPDATE_HTH_SHIFT);
  trimValue |= (((ui32Fcfg1Value &
                      FCFG1_AMPCOMP_TH2_ADC_COMP_AMPTH_LPM_MASK) >>
                     FCFG1_AMPCOMP_TH2_ADC_COMP_AMPTH_LPM_SHIFT) <<
                    DDI0_OSC_AMPCOMPTH2_ADC_COMP_AMPTH_LPM_SHIFT);
  trimValue |= (((ui32Fcfg1Value &
                      FCFG1_AMPCOMP_TH2_ADC_COMP_AMPTH_HPM_MASK) >>
                     FCFG1_AMPCOMP_TH2_ADC_COMP_AMPTH_HPM_SHIFT) <<
                    DDI0_OSC_AMPCOMPTH2_ADC_COMP_AMPTH_HPM_SHIFT);

  return (trimValue);
}

/************************************************************************************
 * Name: rom_setup_get_trim_ampcompth1
 ************************************************************************************/

uint32_t rom_setup_get_trim_ampcompth1(void)
{
  uint32_t trimValue;
  uint32_t ui32Fcfg1Value;

  /* Use device specific trim values located in factory configuration area. All
   * defined register bit fields have a corresponding trim value in the factory
   * configuration area */

  ui32Fcfg1Value = getreg32(TIVA_FCFG1_AMPCOMP_TH1);
  trimValue = (((ui32Fcfg1Value &
                     FCFG1_AMPCOMP_TH1_HPMRAMP3_LTH_MASK) >>
                    FCFG1_AMPCOMP_TH1_HPMRAMP3_LTH_SHIFT) <<
                   DDI0_OSC_AMPCOMPTH1_HPMRAMP3_LTH_SHIFT);
  trimValue |= (((ui32Fcfg1Value &
                      FCFG1_AMPCOMP_TH1_HPMRAMP3_HTH_MASK) >>
                     FCFG1_AMPCOMP_TH1_HPMRAMP3_HTH_SHIFT) <<
                    DDI0_OSC_AMPCOMPTH1_HPMRAMP3_HTH_SHIFT);
  trimValue |= (((ui32Fcfg1Value &
                      FCFG1_AMPCOMP_TH1_IBIASCAP_LPTOHP_OL_CNT_MASK) >>
                     FCFG1_AMPCOMP_TH1_IBIASCAP_LPTOHP_OL_CNT_SHIFT) <<
                    DDI0_OSC_AMPCOMPTH1_IBIASCAP_LPTOHP_OL_CNT_SHIFT);
  trimValue |= (((ui32Fcfg1Value &
                      FCFG1_AMPCOMP_TH1_HPMRAMP1_TH_MASK) >>
                     FCFG1_AMPCOMP_TH1_HPMRAMP1_TH_SHIFT) <<
                    DDI0_OSC_AMPCOMPTH1_HPMRAMP1_TH_SHIFT);

  return (trimValue);
}

/************************************************************************************
 * Name: rom_setup_get_trim_ampcompctrl
 ************************************************************************************/

uint32_t rom_setup_get_trim_ampcompctrl(uint32_t fcfg1_revision)
{
  uint32_t trimValue;
  uint32_t ui32Fcfg1Value;
  uint32_t ibiasOffset;
  uint32_t ibiasInit;
  uint32_t modeConf1;
  int32_t deltaAdjust;

  /* Use device specific trim values located in factory configuration area.
   * Register bit fields without trim values in the factory configuration area
   * will be set to the value of 0. */

  ui32Fcfg1Value = getreg32(TIVA_FCFG1_AMPCOMP_CTRL1);

  ibiasOffset = (ui32Fcfg1Value &
                 FCFG1_AMPCOMP_CTRL1_IBIAS_OFFSET_MASK) >>
    FCFG1_AMPCOMP_CTRL1_IBIAS_OFFSET_SHIFT;
  ibiasInit = (ui32Fcfg1Value &
               FCFG1_AMPCOMP_CTRL1_IBIAS_INIT_MASK) >>
    FCFG1_AMPCOMP_CTRL1_IBIAS_INIT_SHIFT;

  if ((getreg32(TIVA_CCFG_SIZE_AND_DIS_FLAGS) &
       CCFG_SIZE_AND_DIS_FLAGS_DIS_XOSC_OVR_MASK) == 0)
    {
      /* Adjust with TIVA_DELTA_IBIAS_OFFSET and DELTA_IBIAS_INIT from CCFG */

      modeConf1 = getreg32(TIVA_CCFG_MODE_CONF_1);

      /* Both fields are signed 4-bit values. This is an assumption when doing
       * the sign extension. */

      deltaAdjust =
        (((int32_t)
          (modeConf1 <<
           (32 - CCFG_MODE_CONF_1_DELTA_IBIAS_OFFSET_WIDTH -
            CCFG_MODE_CONF_1_DELTA_IBIAS_OFFSET_SHIFT))) >>
            (32 - CCFG_MODE_CONF_1_DELTA_IBIAS_OFFSET_WIDTH));
      deltaAdjust += (int32_t) ibiasOffset;
      if (deltaAdjust < 0)
        {
          deltaAdjust = 0;
        }
      if (deltaAdjust >
          (DDI0_OSC_AMPCOMPCTL_IBIAS_OFFSET_MASK >>
           DDI0_OSC_AMPCOMPCTL_IBIAS_OFFSET_SHIFT))
        {
          deltaAdjust =
            (DDI0_OSC_AMPCOMPCTL_IBIAS_OFFSET_MASK >>
             DDI0_OSC_AMPCOMPCTL_IBIAS_OFFSET_SHIFT);
        }
      ibiasOffset = (uint32_t) deltaAdjust;

      deltaAdjust =
        (((int32_t)
          (modeConf1 <<
           (32 - CCFG_MODE_CONF_1_DELTA_IBIAS_INIT_WIDTH -
            CCFG_MODE_CONF_1_DELTA_IBIAS_INIT_SHIFT))) >> (32 -
                                                       CCFG_MODE_CONF_1_DELTA_IBIAS_INIT_WIDTH));
      deltaAdjust += (int32_t)ibiasInit;
      if (deltaAdjust < 0)
        {
          deltaAdjust = 0;
        }
      if (deltaAdjust >
          (DDI0_OSC_AMPCOMPCTL_IBIAS_INIT_MASK >>
           DDI0_OSC_AMPCOMPCTL_IBIAS_INIT_SHIFT))
        {
          deltaAdjust =
            (DDI0_OSC_AMPCOMPCTL_IBIAS_INIT_MASK >>
             DDI0_OSC_AMPCOMPCTL_IBIAS_INIT_SHIFT);
        }
      ibiasInit = (uint32_t) deltaAdjust;
    }
  trimValue = (ibiasOffset << DDI0_OSC_AMPCOMPCTL_IBIAS_OFFSET_SHIFT) |
    (ibiasInit << DDI0_OSC_AMPCOMPCTL_IBIAS_INIT_SHIFT);

  trimValue |= (((ui32Fcfg1Value &
                      FCFG1_AMPCOMP_CTRL1_LPM_IBIAS_WAIT_CNT_FINAL_MASK) >>
                     FCFG1_AMPCOMP_CTRL1_LPM_IBIAS_WAIT_CNT_FINAL_SHIFT) <<
                    DDI0_OSC_AMPCOMPCTL_LPM_IBIAS_WAIT_CNT_FINAL_SHIFT);
  trimValue |= (((ui32Fcfg1Value &
                      FCFG1_AMPCOMP_CTRL1_CAP_STEP_MASK) >>
                     FCFG1_AMPCOMP_CTRL1_CAP_STEP_SHIFT) <<
                    DDI0_OSC_AMPCOMPCTL_CAP_STEP_SHIFT);
  trimValue |= (((ui32Fcfg1Value &
                      FCFG1_AMPCOMP_CTRL1_IBIASCAP_HPTOLP_OL_CNT_MASK) >>
                     FCFG1_AMPCOMP_CTRL1_IBIASCAP_HPTOLP_OL_CNT_SHIFT) <<
                    DDI0_OSC_AMPCOMPCTL_IBIASCAP_HPTOLP_OL_CNT_SHIFT);

  if (fcfg1_revision >= 0x00000022)
    {
      trimValue |= (((ui32Fcfg1Value &
                          FCFG1_AMPCOMP_CTRL1_AMPCOMP_REQ_MODE_MASK) >>
                         FCFG1_AMPCOMP_CTRL1_AMPCOMP_REQ_MODE_SHIFT) <<
                        DDI0_OSC_AMPCOMPCTL_AMPCOMP_REQ_MODE_SHIFT);
    }

  return (trimValue);
}

/************************************************************************************
 * Name: rom_setup_get_trim_dblrloopfilter_resetvoltage
 ************************************************************************************/

uint32_t rom_setup_get_trim_dblrloopfilter_resetvoltage(uint32_t fcfg1_revision)
{
  uint32_t dblrLoopFilterResetVoltageValue = 0; /* Reset value */

  if (fcfg1_revision >= 0x00000020)
    {
      dblrLoopFilterResetVoltageValue =
        (getreg32(TIVA_FCFG1_MISC_OTP_DATA_1) &
         FCFG1_MISC_OTP_DATA_1_DBLR_LOOP_FILTER_RESET_VOLTAGE_MASK) >>
        FCFG1_MISC_OTP_DATA_1_DBLR_LOOP_FILTER_RESET_VOLTAGE_SHIFT;
    }

  return (dblrLoopFilterResetVoltageValue);
}

/************************************************************************************
 * Name: rom_setup_get_trim_adcshmodeen
 ************************************************************************************/

uint32_t rom_setup_get_trim_adcshmodeen(uint32_t fcfg1_revision)
{
  uint32_t getTrimForAdcShModeEnValue = 1;      /* Recommended default setting */

  if (fcfg1_revision >= 0x00000022)
    {
      getTrimForAdcShModeEnValue = (getreg32(TIVA_FCFG1_OSC_CONF) &
                                    FCFG1_OSC_CONF_ADC_SH_MODE_EN_MASK) >>
        FCFG1_OSC_CONF_ADC_SH_MODE_EN_SHIFT;
    }

  return (getTrimForAdcShModeEnValue);
}

/************************************************************************************
 * Name: rom_setup_get_trim_adcshvbufen
 ************************************************************************************/

uint32_t rom_setup_get_trim_adcshvbufen(uint32_t fcfg1_revision)
{
  uint32_t getTrimForAdcShVbufEnValue = 1;      /* Recommended default setting */

  if (fcfg1_revision >= 0x00000022)
    {
      getTrimForAdcShVbufEnValue = (getreg32(TIVA_FCFG1_OSC_CONF) &
                                    FCFG1_OSC_CONF_ADC_SH_VBUF_EN_MASK) >>
        FCFG1_OSC_CONF_ADC_SH_VBUF_EN_SHIFT;
    }

  return (getTrimForAdcShVbufEnValue);
}

/************************************************************************************
 * Name: rom_setup_get_trim_xosc_hfctrl
 ************************************************************************************/

uint32_t rom_setup_get_trim_xosc_hfctrl(uint32_t fcfg1_revision)
{
  uint32_t getTrimForXoschfCtlValue = 0;        /* Recommended default setting */

  uint32_t fcfg1Data;

  if (fcfg1_revision >= 0x00000020)
    {
      fcfg1Data = getreg32(TIVA_FCFG1_MISC_OTP_DATA_1);
      getTrimForXoschfCtlValue =
        (((fcfg1Data & FCFG1_MISC_OTP_DATA_1_PEAK_DET_ITRIM_MASK) >>
          FCFG1_MISC_OTP_DATA_1_PEAK_DET_ITRIM_SHIFT) <<
         DDI0_OSC_XOSCHFCTL_PEAK_DET_ITRIM_SHIFT);

      getTrimForXoschfCtlValue |=
        (((fcfg1Data & FCFG1_MISC_OTP_DATA_1_HP_BUF_ITRIM_MASK) >>
          FCFG1_MISC_OTP_DATA_1_HP_BUF_ITRIM_SHIFT) <<
         DDI0_OSC_XOSCHFCTL_HP_BUF_ITRIM_SHIFT);

      getTrimForXoschfCtlValue |=
        (((fcfg1Data & FCFG1_MISC_OTP_DATA_1_LP_BUF_ITRIM_MASK) >>
          FCFG1_MISC_OTP_DATA_1_LP_BUF_ITRIM_SHIFT) <<
         DDI0_OSC_XOSCHFCTL_LP_BUF_ITRIM_SHIFT);
    }

  return (getTrimForXoschfCtlValue);
}

/************************************************************************************
 * Name: rom_setup_get_trim_xosc_hffaststart
 ************************************************************************************/

uint32_t rom_setup_get_trim_xosc_hffaststart(void)
{
  uint32_t ui32XoscHfFastStartValue;

  /* Get value from FCFG1 */

  ui32XoscHfFastStartValue = (getreg32(TIVA_FCFG1_OSC_CONF) &
                              FCFG1_OSC_CONF_XOSC_HF_FAST_START_MASK) >>
    FCFG1_OSC_CONF_XOSC_HF_FAST_START_SHIFT;

  return (ui32XoscHfFastStartValue);
}

/************************************************************************************
 * Name: rom_setup_get_trim_radc_extcfg
 ************************************************************************************/

uint32_t rom_setup_get_trim_radc_extcfg(uint32_t fcfg1_revision)
{
  uint32_t getTrimForRadcExtCfgValue = 0x403f8000;      /* Recommended default
                                                         * setting */

  uint32_t fcfg1Data;

  if (fcfg1_revision >= 0x00000020)
    {
      fcfg1Data = getreg32(TIVA_FCFG1_MISC_OTP_DATA_1);
      getTrimForRadcExtCfgValue =
        (((fcfg1Data & FCFG1_MISC_OTP_DATA_1_HPM_IBIAS_WAIT_CNT_MASK) >>
          FCFG1_MISC_OTP_DATA_1_HPM_IBIAS_WAIT_CNT_SHIFT) <<
         DDI0_OSC_RADCEXTCFG_HPM_IBIAS_WAIT_CNT_SHIFT);

      getTrimForRadcExtCfgValue |=
        (((fcfg1Data & FCFG1_MISC_OTP_DATA_1_LPM_IBIAS_WAIT_CNT_MASK) >>
          FCFG1_MISC_OTP_DATA_1_LPM_IBIAS_WAIT_CNT_SHIFT) <<
         DDI0_OSC_RADCEXTCFG_LPM_IBIAS_WAIT_CNT_SHIFT);

      getTrimForRadcExtCfgValue |=
        (((fcfg1Data & FCFG1_MISC_OTP_DATA_1_IDAC_STEP_MASK) >>
          FCFG1_MISC_OTP_DATA_1_IDAC_STEP_SHIFT) <<
         DDI0_OSC_RADCEXTCFG_IDAC_STEP_SHIFT);
    }

  return (getTrimForRadcExtCfgValue);
}

/************************************************************************************
 * Name: rom_setup_get_trim_rcosc_lfibiastrim
 ************************************************************************************/

uint32_t rom_setup_get_trim_rcosc_lfibiastrim(uint32_t fcfg1_revision)
{
  uint32_t trimForRcOscLfIBiasTrimValue = 0;    /* Default value */

  if (fcfg1_revision >= 0x00000022)
    {
      trimForRcOscLfIBiasTrimValue = (getreg32(TIVA_FCFG1_OSC_CONF) &
                                      FCFG1_OSC_CONF_ATESTLF_RCOSCLF_IBIAS_TRIM_MASK)
        >> FCFG1_OSC_CONF_ATESTLF_RCOSCLF_IBIAS_TRIM_SHIFT;
    }

  return (trimForRcOscLfIBiasTrimValue);
}

/************************************************************************************
 * Name: rom_setup_get_trim_lfregulator_cmirrwr_ratio
 ************************************************************************************/

uint32_t rom_setup_get_trim_lfregulator_cmirrwr_ratio(uint32_t fcfg1_revision)
{
  /* Default value for both fields */

  uint32_t trimForXoscLfRegulatorAndCmirrwrRatioValue = 0;

  if (fcfg1_revision >= 0x00000022)
    {
      trimForXoscLfRegulatorAndCmirrwrRatioValue =
        (getreg32(TIVA_FCFG1_OSC_CONF) &
         (FCFG1_OSC_CONF_XOSCLF_REGULATOR_TRIM_MASK |
          FCFG1_OSC_CONF_XOSCLF_CMIRRWR_RATIO_MASK)) >>
        FCFG1_OSC_CONF_XOSCLF_CMIRRWR_RATIO_SHIFT;
    }

  return (trimForXoscLfRegulatorAndCmirrwrRatioValue);
}

/************************************************************************************
 * Name: rom_setup_cachemode
 ************************************************************************************/

void rom_setup_cachemode(void)
{
  /* - Make sure to enable aggressive VIMS clock gating for power optimization
   * Only for PG2 devices. - Enable cache prefetch enable as default setting
   * (Slightly higher power consumption, but higher CPU performance) - IF (
   * CCFG_..._DIS_GPRAM == 1 ) then: Enable cache (set cache mode = 1), even if
   * set by ROM boot code (This is done because it's not set by boot code when
   * running inside a debugger supporting the Halt In Boot (HIB)
   * functionality).  else: Set MODE_GPRAM if not already set (see inline
   * comments as well)
   */

  uint32_t vims_ctlmode0;

  while ((getreg32(TIVA_VIMS_STAT) & VIMS_STAT_MODE_CHANGING) != 0)
    {
      /* Do nothing - wait for an eventual ongoing mode change to complete.
       * (There should typically be no wait time here, but need to be sure)
       */
    }

  /* Note that Mode=0 is equal to MODE_GPRAM */

  vims_ctlmode0 =
    ((getreg32(TIVA_VIMS_CTL) & ~VIMS_CTL_MODE_MASK) | VIMS_CTL_DYN_CG_EN |
     VIMS_CTL_PREF_EN);

  if (getreg32(TIVA_CCFG_SIZE_AND_DIS_FLAGS) &
      CCFG_SIZE_AND_DIS_FLAGS_DIS_GPRAM)
    {
      /* Enable cache (and hence disable GPRAM) */

      putreg32((vims_ctlmode0 | VIMS_CTL_MODE_CACHE), TIVA_VIMS_CTL);
    }
  else if ((getreg32(TIVA_VIMS_STAT) & VIMS_STAT_MODE_MASK) !=
           VIMS_STAT_MODE_GPRAM)
    {
      /* GPRAM is enabled in CCFG but not selected Note: It is recommended to
       * go via MODE_OFF when switching to MODE_GPRAM */

      putreg32((vims_ctlmode0 | VIMS_CTL_MODE_OFF), TIVA_VIMS_CTL);
      while ((getreg32(TIVA_VIMS_STAT) & VIMS_STAT_MODE_MASK) !=
             VIMS_STAT_MODE_OFF)
        {
          /* Do nothing - wait for an eventual mode change to complete (This
           * goes fast). */

        }

      putreg32(vims_ctlmode0, TIVA_VIMS_CTL);
    }
  else
    {
      /* Correct mode, but make sure PREF_EN and DYN_CG_EN always are set */

      putreg32(vims_ctlmode0, TIVA_VIMS_CTL);
    }
}

/************************************************************************************
 * Name: rom_setup_aonrtc_subsecinc
 ************************************************************************************/

void rom_setup_aonrtc_subsecinc(uint32_t subsecinc)
{
  /* Loading a new RTCSUBSECINC value is done in 5 steps: 1. Write bit[15:0] of
   * new SUBSECINC value to TIVA_AUX_WUC_RTCSUBSECINC0 2. Write bit[23:16] of new
   * SUBSECINC value to TIVA_AUX_WUC_RTCSUBSECINC1 3. Set
   * AUX_WUC_RTCSUBSECINCCTL_UPD_REQ 4. Wait for
   * AUX_WUC_RTCSUBSECINCCTL_UPD_ACK 5. Clear AUX_WUC_RTCSUBSECINCCTL_UPD_REQ */

  putreg32(((subsecinc) & AUX_WUC_RTCSUBSECINC0_INC15_0_MASK),
           TIVA_AUX_WUC_RTCSUBSECINC0);
  putreg32(((subsecinc >> 16) & AUX_WUC_RTCSUBSECINC1_INC23_16_MASK),
           TIVA_AUX_WUC_RTCSUBSECINC1);

  putreg32(AUX_WUC_RTCSUBSECINCCTL_UPD_REQ, TIVA_AUX_WUC_RTCSUBSECINCCTL);
  while ((getreg32(TIVA_AUX_WUC_RTCSUBSECINCCTL) & AUX_WUC_RTCSUBSECINCCTL_UPD_ACK) == 0)
    {
    }

  putreg32(0, TIVA_AUX_WUC_RTCSUBSECINCCTL);
}
