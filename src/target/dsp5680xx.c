/***************************************************************************
 *   Copyright (C) 2011 by Rodrigo L. Rosa                                 *
 *   rodrigorosa.LG@gmail.com                                              *
 *                                                                         *
 *   Based on dsp563xx_once.h written by Mathias Kuester                   *
 *   mkdorg@users.sourceforge.net                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "target.h"
#include "target_type.h"
#include "dsp5680xx.h"

#define err_check(retval,err_msg) if(retval != ERROR_OK){LOG_ERROR("%s: %d %s.",__FUNCTION__,__LINE__,err_msg);return retval;}
#define err_check_propagate(retval) if(retval!=ERROR_OK){return retval;}

// Forward declarations, could try to optimize this.
static int eonce_instruction_exec(struct target * target, uint8_t instr, uint8_t rw, uint8_t go, uint8_t ex, uint8_t * eonce_status);
static int eonce_load_TX_RX_to_r0(struct target * target);
static int eonce_enter_debug_mode(struct target * target, uint16_t * eonce_status);
static int eonce_read_status_reg(struct target * target, uint16_t * data);
static int eonce_pc_store(struct target * target);
static int eonce_move_value_to_pc(struct target * target, uint32_t value);
static int dsp5680xx_jtag_status(struct target *target, uint8_t * status);
static int dsp5680xx_resume(struct target *target, int current, uint32_t address,int handle_breakpoints, int debug_execution);
static int dsp5680xx_halt(struct target *target);
static int dsp5680xx_write(struct target *target, uint32_t address, uint32_t size, uint32_t count, const uint8_t * buffer);

int dsp5680xx_execute_queue(void){
  int retval;
  retval = jtag_execute_queue();
  err_check_propagate(retval);
  return retval;
}

static int eonce_exit_debug_mode(struct target * target,uint8_t * eonce_status){
  int retval;
  retval = eonce_instruction_exec(target,0x1F,0,0,1,eonce_status);
  err_check_propagate(retval);
  return retval;
}


static int dsp5680xx_drscan(struct target * target, uint8_t * data_to_shift_into_dr, uint8_t * data_shifted_out_of_dr, int len){
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
//
// Inputs:
//     - data_to_shift_into_dr: This is the data that will be shifted into the JTAG DR reg.
//     - data_shifted_out_of_dr: The data that will be shifted out of the JTAG DR reg will stored here
//     - len: Length of the data to be shifted to JTAG DR.
//
// Note:  If  data_shifted_out_of_dr  == NULL, discard incoming bits.
//
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  int retval = ERROR_OK;
  if (NULL == target->tap){
	retval = ERROR_FAIL;
	err_check(retval,"Invalid tap");
  }
  if (len > 32){
	retval = ERROR_FAIL;
	err_check(retval,"dr_len overflow, maxium is 32");
  }
  //TODO what values of len are valid for jtag_add_plain_dr_scan?
  //can i send as many bits as i want?
  //is the casting necessary?
  jtag_add_plain_dr_scan(len,data_to_shift_into_dr,data_shifted_out_of_dr, TAP_IDLE);
  if(context.flush){
	retval = dsp5680xx_execute_queue();
	err_check_propagate(retval);
  }
  if(data_shifted_out_of_dr!=NULL){
    LOG_DEBUG("Data read (%d bits): 0x%04X",len,*data_shifted_out_of_dr);
  }else
    LOG_DEBUG("Data read was discarded.");
  return retval;
}

static int dsp5680xx_irscan(struct target * target, uint32_t * data_to_shift_into_ir, uint32_t * data_shifted_out_of_ir, uint8_t ir_len){
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
// Inputs:
//     - data_to_shift_into_ir: This is the data that will be shifted into the JTAG IR reg.
//     - data_shifted_out_of_ir: The data that will be shifted out of the JTAG IR reg will stored here
//     - len: Length of the data to be shifted to JTAG IR.
//
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  int retval = ERROR_OK;
  if (NULL == target->tap){
	retval = ERROR_FAIL;
	err_check(retval,"Invalid tap");
  }
  if (ir_len != target->tap->ir_length){
    LOG_WARNING("%s: Invalid ir_len of core tap. If you are removing protection on flash then do not worry about this warninig.",__FUNCTION__);
    //return ERROR_FAIL;//TODO this was commented out to enable unlocking using the master tap. did not find a way to enable the master tap without using tcl.
  }
  //TODO what values of len are valid for jtag_add_plain_ir_scan?
  //can i send as many bits as i want?
  //is the casting necessary?
  jtag_add_plain_ir_scan(ir_len,(uint8_t *)data_to_shift_into_ir,(uint8_t *)data_shifted_out_of_ir, TAP_IDLE);
  if(context.flush){
	retval = dsp5680xx_execute_queue();
	err_check_propagate(retval);
  }
  return retval;
}

static int dsp5680xx_read_core_reg(struct target * target, uint8_t reg_addr, uint16_t * data_read)
{
  //TODO implement a general version of this which matches what openocd uses.
  int retval;
  uint32_t dummy_data_to_shift_into_dr;
  retval = eonce_instruction_exec(target,reg_addr,1,0,0,NULL);
  err_check_propagate(retval);
  retval = dsp5680xx_drscan(target,(uint8_t *)& dummy_data_to_shift_into_dr,(uint8_t *) data_read, 8);
  err_check_propagate(retval);
  LOG_DEBUG("Reg. data: 0x%02X.",*data_read);
  return retval;
}

static int dsp5680xx_target_create(struct target *target, Jim_Interp * interp){
  struct dsp5680xx_common *dsp5680xx = calloc(1, sizeof(struct dsp5680xx_common));
  target->arch_info = dsp5680xx;
  return ERROR_OK;
}

static int dsp5680xx_init_target(struct command_context *cmd_ctx, struct target *target){
  context.stored_pc = 0;
  context.flush = 1;
  LOG_DEBUG("target initiated!");
  //TODO core tap must be enabled before running these commands, currently this is done in the .cfg tcl script.
  return ERROR_OK;
}

static int dsp5680xx_arch_state(struct target *target){
  LOG_USER("%s not implemented yet.",__FUNCTION__);
  return ERROR_OK;
}

int dsp5680xx_target_status(struct target * target, uint8_t * jtag_st, uint16_t * eonce_st){
  return target->state;
}

static int dsp5680xx_assert_reset(struct target *target){
  target->state = TARGET_RESET;
  return ERROR_OK;
}

static int dsp5680xx_deassert_reset(struct target *target){
  target->state = TARGET_RUNNING;
  return ERROR_OK;
}

static int dsp5680xx_poll(struct target *target){
  int retval;
  uint8_t jtag_status;
  uint8_t eonce_status;
  uint16_t read_tmp;
  retval = dsp5680xx_jtag_status(target,&jtag_status);
  err_check_propagate(retval);
  if (jtag_status == JTAG_STATUS_DEBUG)
    if (target->state != TARGET_HALTED){
      retval = eonce_enter_debug_mode(target,&read_tmp);
	  err_check_propagate(retval);
      eonce_status = (uint8_t) read_tmp;
      if((eonce_status&EONCE_STAT_MASK) != DSP5680XX_ONCE_OSCR_DEBUG_M){
		LOG_WARNING("%s: Failed to put EOnCE in debug mode. Is flash locked?...",__FUNCTION__);
		return ERROR_TARGET_FAILURE;
      }else{
		target->state = TARGET_HALTED;
		return ERROR_OK;
      }
    }
  if (jtag_status == JTAG_STATUS_NORMAL){
    if(target->state == TARGET_RESET){
      retval = dsp5680xx_halt(target);
	  err_check_propagate(retval);
      retval = eonce_exit_debug_mode(target,&eonce_status);
	  err_check_propagate(retval);
      if((eonce_status&EONCE_STAT_MASK) != DSP5680XX_ONCE_OSCR_NORMAL_M){
		LOG_WARNING("%s: JTAG running, but cannot make EOnCE run. Try resetting...",__FUNCTION__);
		return ERROR_TARGET_FAILURE;
      }else{
		target->state = TARGET_RUNNING;
		return ERROR_OK;
      }
    }
    if(target->state != TARGET_RUNNING){
      retval = eonce_read_status_reg(target,&read_tmp);
	  err_check_propagate(retval);
      eonce_status = (uint8_t) read_tmp;
      if((eonce_status&EONCE_STAT_MASK) != DSP5680XX_ONCE_OSCR_NORMAL_M){
		LOG_WARNING("Inconsistent target status. Restart!");
		return ERROR_TARGET_FAILURE;
      }
    }
    target->state = TARGET_RUNNING;
    return ERROR_OK;
  }
  if(jtag_status == JTAG_STATUS_DEAD){
    LOG_ERROR("%s: Cannot communicate with JTAG. Check connection...",__FUNCTION__);
    target->state = TARGET_UNKNOWN;
    return ERROR_TARGET_FAILURE;
  };
  if (target->state == TARGET_UNKNOWN){
    LOG_ERROR("%s: Target status invalid - communication failure",__FUNCTION__);
    return ERROR_TARGET_FAILURE;
  };
  return ERROR_OK;
}

static int dsp5680xx_jtag_status(struct target *target, uint8_t * status){
  uint32_t read_from_ir;
  uint32_t instr;
  int retval;
  instr =  JTAG_INSTR_ENABLE_ONCE;
  retval = dsp5680xx_irscan(target,& instr, & read_from_ir,DSP5680XX_JTAG_CORE_TAP_IRLEN);
  err_check_propagate(retval);
  if(status!=NULL)
    *status = (uint8_t)read_from_ir;
  return ERROR_OK;
}

static int eonce_read_status_reg(struct target * target, uint16_t * data){
  int retval;
  retval = dsp5680xx_read_core_reg(target,DSP5680XX_ONCE_OSR,data);
  err_check_propagate(retval);
  return retval;
}

static int dsp5680xx_obase_addr(struct target * target, uint32_t * addr){
  // Finds out the default value of the OBASE register address.
  int retval;
  uint32_t data_to_shift_into_dr;// just to make jtag happy
  retval = eonce_instruction_exec(target,DSP5680XX_ONCE_OBASE,1,0,0,NULL);
  err_check_propagate(retval);
  retval = dsp5680xx_drscan(target,(uint8_t *)& data_to_shift_into_dr,(uint8_t *) addr, 8);
  err_check_propagate(retval);
  return retval;
}

static int dsp5680xx_halt(struct target *target){
  int retval;
  uint8_t jtag_status;
  uint16_t eonce_status;
  if(target->state == TARGET_HALTED){
    LOG_USER("Target already halted.");
    return ERROR_OK;
  }
  retval = eonce_enter_debug_mode(target,&eonce_status);
  err_check_propagate(retval);
  retval = dsp5680xx_jtag_status(target,&jtag_status);
  err_check_propagate(retval);
  retval = eonce_pc_store(target);
  err_check_propagate(retval);
  //TODO is it useful to store the pc?
  return retval;
}

static int dsp5680xx_resume(struct target *target, int current, uint32_t address,int handle_breakpoints, int debug_execution){
  if(target->state == TARGET_RUNNING){
    LOG_USER("Target already running.");
    return ERROR_OK;
  }
  int retval;
  uint8_t jtag_status;
  uint16_t eonce_status;

  // Verify that EOnCE is enabled (enable it if necessary)
  uint16_t data_read_from_dr = 0;
  retval = eonce_read_status_reg(target,&data_read_from_dr);
  err_check_propagate(retval);
  if((data_read_from_dr&DSP5680XX_ONCE_OSCR_DEBUG_M) != DSP5680XX_ONCE_OSCR_DEBUG_M){
    retval = eonce_enter_debug_mode(target,NULL);
	err_check_propagate(retval);
  }
  if(!current){
    retval = eonce_move_value_to_pc(target,address);
    err_check_propagate(retval);
  }

  int retry = 20;
  while(retry-- > 1){
    retval = eonce_exit_debug_mode(target,(uint8_t *)&eonce_status );
	err_check_propagate(retval);
    retval = dsp5680xx_jtag_status(target,&jtag_status);
	err_check_propagate(retval);
    if((jtag_status & 0xff) == JTAG_STATUS_NORMAL){
      break;
    }
  }
  if(retry == 0){
    retval = ERROR_TARGET_FAILURE;
	err_check(retval,"Failed to resume...");
  }else{
    target->state = TARGET_RUNNING;
  };
  LOG_DEBUG("JTAG status: 0x%02X.",jtag_status);
  LOG_DEBUG("EOnCE status: 0x%02X.",eonce_status);
  return ERROR_OK;
}

static int jtag_data_read(struct target * target, uint32_t * data_read, int num_bits){
  uint32_t bogus_instr;
  int retval = dsp5680xx_drscan(target,(uint8_t *) & bogus_instr,(uint8_t *) data_read,num_bits);
  LOG_DEBUG("Data read (%d bits): 0x%04X",num_bits,*data_read);//TODO remove this or move to jtagio?
  return retval;
}

#define jtag_data_read8(target,data_read)  jtag_data_read(target,data_read,8)
#define jtag_data_read16(target,data_read) jtag_data_read(target,data_read,16)
#define jtag_data_read32(target,data_read) jtag_data_read(target,data_read,32)

static int jtag_data_write(struct target * target, uint32_t instr,int num_bits, uint32_t * data_read){
  int retval;
  uint32_t data_read_dummy;
  retval = dsp5680xx_drscan(target,(uint8_t *) & instr,(uint8_t *) & data_read_dummy,num_bits);
  err_check_propagate(retval);
  if(data_read != NULL)
    *data_read = data_read_dummy;
  return retval;
}

#define jtag_data_write8(target,instr,data_read)  jtag_data_write(target,instr,8,data_read)
#define jtag_data_write16(target,instr,data_read) jtag_data_write(target,instr,16,data_read)
#define jtag_data_write24(target,instr,data_read) jtag_data_write(target,instr,24,data_read)
#define jtag_data_write32(target,instr,data_read) jtag_data_write(target,instr,32,data_read)

static int eonce_enter_debug_mode(struct target * target, uint16_t * eonce_status){
  int retval;
  uint32_t instr = JTAG_INSTR_DEBUG_REQUEST;
  uint32_t ir_out;//not used, just to make jtag happy.
  // Debug request #1
  retval = dsp5680xx_irscan(target,& instr,& ir_out,DSP5680XX_JTAG_CORE_TAP_IRLEN);
  err_check_propagate(retval);

  // Enable EOnCE module
  instr = JTAG_INSTR_ENABLE_ONCE;
  //Two rounds of jtag 0x6  (enable eonce) to enable EOnCE.
  retval =  dsp5680xx_irscan(target, & instr, & ir_out,DSP5680XX_JTAG_CORE_TAP_IRLEN);
  err_check_propagate(retval);
  retval =  dsp5680xx_irscan(target, & instr, & ir_out,DSP5680XX_JTAG_CORE_TAP_IRLEN);
  err_check_propagate(retval);
  // Verify that debug mode is enabled
  uint16_t data_read_from_dr;
  retval = eonce_read_status_reg(target,&data_read_from_dr);
  err_check_propagate(retval);
  if((data_read_from_dr&0x30) == 0x30){
    LOG_DEBUG("EOnCE successfully entered debug mode.");
    target->state = TARGET_HALTED;
    return ERROR_OK;
  }else{
	retval = ERROR_TARGET_FAILURE;
	err_check(retval,"Failed to set EOnCE module to debug mode.");
  }
  if(eonce_status!=NULL)
    *eonce_status = data_read_from_dr;
  return ERROR_OK;
}

static int eonce_instruction_exec(struct target * target, uint8_t instr, uint8_t rw, uint8_t go, uint8_t ex,uint8_t * eonce_status){
  int retval;
  uint32_t dr_out_tmp;
  uint8_t instr_with_flags = instr|(rw<<7)|(go<<6)|(ex<<5);
  retval = jtag_data_write(target,instr_with_flags,8,&dr_out_tmp);
  err_check_propagate(retval);
  if(eonce_status != NULL)
    *eonce_status =  (uint8_t) dr_out_tmp;
  return retval;
}

/* Executes DSP instruction */
/* wrappers for parameter conversion between eonce_execute_instruction and eonce_execute_instructionX */
#define eonce_execute_instruction_1(target,opcode1,opcode2,opcode3)	 eonce_execute_instruction1(target,opcode1)
#define eonce_execute_instruction_2(target,opcode1,opcode2,opcode3)	 eonce_execute_instruction2(target,opcode1,opcode2)
#define eonce_execute_instruction_3(target,opcode1,opcode2,opcode3)	 eonce_execute_instruction3(target,opcode1,opcode2,opcode3)
/* the macro itself */
#define eonce_execute_instruction(target,words,opcode1,opcode2,opcode3) eonce_execute_instruction_##words(target,opcode1,opcode2,opcode3)

/* Executes one word DSP instruction */
static int eonce_execute_instruction1(struct target * target, uint16_t opcode){
  int retval;
  retval = eonce_instruction_exec(target,0x04,0,1,0,NULL);
  err_check_propagate(retval);
  retval = jtag_data_write16(target,opcode,NULL);
  err_check_propagate(retval);
  return retval;
}

/* Executes two word DSP instruction */
static int eonce_execute_instruction2(struct target * target,uint16_t opcode1, uint16_t opcode2){
  int retval;
  retval = eonce_instruction_exec(target,0x04,0,0,0,NULL);
  err_check_propagate(retval);
  retval = jtag_data_write16(target,opcode1,NULL);
  err_check_propagate(retval);
  retval = eonce_instruction_exec(target,0x04,0,1,0,NULL);
  err_check_propagate(retval);
  retval = jtag_data_write16(target,opcode2,NULL);
  err_check_propagate(retval);
  return retval;
}

/* Executes three word DSP instruction */
static int eonce_execute_instruction3(struct target * target, uint16_t opcode1,uint16_t opcode2,uint16_t opcode3){
  int retval;
  retval = eonce_instruction_exec(target,0x04,0,0,0,NULL);
  err_check_propagate(retval);
  retval = jtag_data_write16(target,opcode1,NULL);
  err_check_propagate(retval);
  retval = eonce_instruction_exec(target,0x04,0,0,0,NULL);
  err_check_propagate(retval);
  retval = jtag_data_write16(target,opcode2,NULL);
  err_check_propagate(retval);
  retval = eonce_instruction_exec(target,0x04,0,1,0,NULL);
  err_check_propagate(retval);
  retval = jtag_data_write16(target,opcode3,NULL);
  err_check_propagate(retval);
  return retval;
}

/* --------------- Real-time data exchange --------------- */
/*
  The EOnCE Transmit (OTX) and Receive (ORX) registers are data memory mapped, each with an upper and lower 16 bit word.
  Transmit and receive directions are defined from the core’s perspective.
  The core writes to the Transmit register and reads the Receive register, and the host through JTAG writes to the Receive register and reads the Transmit register.
  Both registers have a combined data memory mapped OTXRXSR which provides indication when each may be accessed.
ref: eonce_rev.1.0_0208081.pdf@36
*/

static int eonce_tx_upper_data(struct target * target, uint16_t data, uint32_t * eonce_status_low){
  int retval;
  retval = eonce_instruction_exec(target,DSP5680XX_ONCE_ORX1,0,0,0,NULL);
  err_check_propagate(retval);
  retval = jtag_data_write16(target,data,eonce_status_low);
  err_check_propagate(retval);
  return retval;
}

/* writes data into lower ORx register of the target */
#define eonce_tx_lower_data(target,data) eonce_instruction_exec(target,DSP5680XX_ONCE_ORX,0,0,0,NULL);\
								  jtag_data_write16(target,data)

/**
 *
 * @param target
 * @param data_read: Returns the data read from the upper OTX register via JTAG.
 * @return: Returns an error code (see error code documentation)
 */
static int eonce_rx_upper_data(struct target * target, uint16_t * data_read)
{
  int retval;
  retval = eonce_instruction_exec(target,DSP5680XX_ONCE_OTX1,1,0,0,NULL);
  err_check_propagate(retval);
  retval = jtag_data_read16(target,(uint32_t *)data_read);
  err_check_propagate(retval);
  return retval;
}

/**
 *
 * @param target
 * @param data_read: Returns the data read from the lower OTX register via JTAG.
 * @return: Returns an error code (see error code documentation)
 */
static int eonce_rx_lower_data(struct target * target,uint16_t * data_read)
{
  int retval;
  retval = eonce_instruction_exec(target,DSP5680XX_ONCE_OTX,1,0,0,NULL);
  err_check_propagate(retval);
  retval = jtag_data_read16(target,(uint32_t *)data_read);
  err_check_propagate(retval);
  return retval;
}

/* -- -- -- -- --- -- -- -- --- -- -- -- --- -- -- -- --- -- -- -- --- -- -*/
/* -- -- -- -- --- -- -- -Core Instructions- -- -- -- --- -- -- -- --- -- -*/
/* -- -- -- -- --- -- -- -- --- -- -- -- --- -- -- -- --- -- -- -- --- -- -*/
/* move.l #value,r0 */
#define eonce_move_long_to_r0(target,value)	eonce_execute_instruction(target,3,0xe418,value&0xffff,value>>16)

/* move.l #value,n */
#define eonce_move_long_to_n(target,value)		eonce_execute_instruction(target,3,0xe41e,value&0xffff,value>>16)

/* move x:(r0),y0 */
#define eonce_move_at_r0_to_y0(target)			eonce_execute_instruction(target,1,0xF514,0,0)

/* move x:(r0),y1 */
#define eonce_move_at_r0_to_y1(target)			eonce_execute_instruction(target,1,0xF714,0,0)

/* move.l x:(r0),y */
#define eonce_move_long_at_r0_y(target) eonce_execute_instruction(target,1,0xF734,0,0)

/* move y0,x:(r0) */
#define eonce_move_y0_at_r0(target)			eonce_execute_instruction(target,1,0xd514,0,0)

/* bfclr #value,x:(r0) */
#define eonce_bfclr_at_r0(target,value)		eonce_execute_instruction(target,2,0x8040,value,0)

/* move #value,y0 */
#define eonce_move_value_to_y0(target,value)	eonce_execute_instruction(target,2,0x8745,value,0)

/* move.w y0,x:(r0)+ */
#define eonce_move_y0_at_r0_inc(target)		eonce_execute_instruction(target,1,0xd500,0,0)

/* move.w y0,p:(r0)+ */
#define eonce_move_y0_at_pr0_inc(target)		eonce_execute_instruction(target,1,0x8560,0,0)

/* move.w p:(r0)+,y0 */
#define eonce_move_at_pr0_inc_to_y0(target)	eonce_execute_instruction(target,1,0x8568,0,0)

/* move.w p:(r0)+,y1 */
#define eonce_move_at_pr0_inc_to_y1(target)	eonce_execute_instruction(target,1,0x8768,0,0)

/* move.l #value,r2 */
#define eonce_move_long_to_r2(target,value)	eonce_execute_instruction(target,3,0xe41A,value&0xffff,value>>16)

/* move y0,x:(r2) */
#define eonce_move_y0_at_r2(target)             eonce_execute_instruction(target,1,0xd516,0,0)

/* move.w #<value>,x:(r2) */
#define eonce_move_value_at_r2(target,value)	eonce_execute_instruction(target,2,0x8642,value,0)

/* move.w #<value>,x:(r0) */
#define eonce_move_value_at_r0(target,value)	eonce_execute_instruction(target,2,0x8640,value,0)

/* move.w #<value>,x:(R2+<disp>) */
#define eonce_move_value_at_r2_disp(target,value,disp)	eonce_execute_instruction(target,3,0x8646,value,disp)

/* move.w x:(r2),Y0 */
#define eonce_move_at_r2_to_y0(target)		eonce_execute_instruction(target,1,0xF516,0,0)

/* move.w p:(r2)+,y0 */
#define eonce_move_at_pr2_inc_to_y0(target)	eonce_execute_instruction(target,1,0x856A,0,0)

/* move.l #value,r3 */
#define eonce_move_long_to_r1(target,value)	eonce_execute_instruction(target,3,0xE419,value&0xffff,value>>16)

/* move.l #value,r3 */
#define eonce_move_long_to_r3(target,value)	eonce_execute_instruction(target,3,0xE41B,value&0xffff,value>>16)

/* move.w y0,p:(r3)+ */
#define eonce_move_y0_at_pr3_inc(target)		eonce_execute_instruction(target,1,0x8563,0,0)

/* move.w y0,x:(r3) */
#define eonce_move_y0_at_r3(target)			eonce_execute_instruction(target,1,0xD503,0,0)

/* move pc,r4 */
#define eonce_move_pc_to_r4(target)			eonce_execute_instruction(target,1,0xE716,0,0)

/* move.l r4,y */
#define eonce_move_r4_to_y(target)			eonce_execute_instruction(target,1,0xe764,0,0)

/* move.w p:(r0)+,y0 */
#define eonce_move_at_pr0_inc_to_y0(target)	eonce_execute_instruction(target,1,0x8568,0,0)

/* move.w x:(r0)+,y0 */
#define eonce_move_at_r0_inc_to_y0(target)	eonce_execute_instruction(target,1,0xf500,0,0)

/* move x:(r0),y0 */
#define eonce_move_at_r0_y0(target)			eonce_execute_instruction(target,1,0xF514,0,0)

/* nop */
#define eonce_nop(target)		eonce_execute_instruction(target,1,0xe700,0,0)

/* move.w x:(R2+<disp>),Y0 */
#define eonce_move_at_r2_disp_to_y0(target,disp) eonce_execute_instruction(target,2,0xF542,disp,0)

/* move.w y1,x:(r2) */
#define eonce_move_y1_at_r2(target) eonce_execute_instruction(target,1,0xd716,0,0)

/* move.w y1,x:(r0) */
#define eonce_move_y1_at_r0(target) eonce_execute_instruction(target,1,0xd714,0,0)

/* move.bp y0,x:(r0)+ */
#define eonce_move_byte_y0_at_r0(target) eonce_execute_instruction(target,1,0xd5a0,0,0)

/* move.w y1,p:(r0)+ */
#define eonce_move_y1_at_pr0_inc(target) eonce_execute_instruction(target,1,0x8760,0,0)

/* move.w y1,x:(r0)+ */
#define eonce_move_y1_at_r0_inc(target) eonce_execute_instruction(target,1,0xD700,0,0)

/* move.l #value,y */
#define eonce_move_long_to_y(target,value) eonce_execute_instruction(target,3,0xe417,value&0xffff,value>>16)

static int eonce_move_value_to_pc(struct target * target, uint32_t value)
{
  if (!(target->state == TARGET_HALTED)){
    LOG_ERROR("Target must be halted to move PC. Target state = %d.",target->state);
    return ERROR_TARGET_NOT_HALTED;
  };
  int retval;
  retval = eonce_execute_instruction(target,3,0xE71E,value&0xffff,value>>16);
  err_check_propagate(retval);
  return retval;
}

static int eonce_load_TX_RX_to_r0(struct target * target)
{
  uint32_t obase_addr;
  int retval = dsp5680xx_obase_addr(target,& obase_addr);
  err_check_propagate(retval);
  retval = eonce_move_long_to_r0(target,((MC568013_EONCE_TX_RX_ADDR)+(obase_addr<<16)));
  return retval;
}

static int eonce_load_TX_RX_high_to_r0(struct target * target)
{
  uint32_t obase_addr;
  int retval = dsp5680xx_obase_addr(target,& obase_addr);
  err_check_propagate(retval);
  if(!(obase_addr && 0xff)){
	LOG_USER("%s: OBASE address read as 0x%04X instead of 0xFF.",__FUNCTION__,obase_addr);
	return ERROR_FAIL;
  }
  eonce_move_long_to_r0(target,((MC568013_EONCE_TX1_RX1_HIGH_ADDR)+(obase_addr<<16)));
  err_check_propagate(retval);
  return retval;
}

static int eonce_pc_store(struct target * target){
  uint32_t tmp = 0;
  int retval;
  retval = eonce_move_pc_to_r4(target);
  err_check_propagate(retval);
  retval = eonce_move_r4_to_y(target);
  err_check_propagate(retval);
  retval = eonce_load_TX_RX_to_r0(target);
  err_check_propagate(retval);
  retval = eonce_move_y0_at_r0(target);
  err_check_propagate(retval);
  retval = eonce_rx_lower_data(target,(uint16_t *)&tmp);
  err_check_propagate(retval);
  LOG_USER("PC value: 0x%06X\n",tmp);
  context.stored_pc = (uint32_t)tmp;
  return ERROR_OK;
}

static int dsp5680xx_convert_address(uint32_t * address, int * pmem){
  // Distinguish data memory (x:) from program memory (p:) by the address.
  // Addresses over S_FILE_DATA_OFFSET are considered (x:) memory.
  if(*address >= S_FILE_DATA_OFFSET){
    *pmem = 0;
    if(((*address)&0xff0000)!=0xff0000)
      *address -= S_FILE_DATA_OFFSET;
  }
  return ERROR_OK;
}

static int dsp5680xx_read_16_single(struct target * target, uint32_t address, uint16_t * data_read, int r_pmem){
  //TODO add error control!
  int retval;
  retval = eonce_move_long_to_r0(target,address);
  err_check_propagate(retval);
  if(r_pmem)
    retval = eonce_move_at_pr0_inc_to_y0(target);
  else
    retval = eonce_move_at_r0_to_y0(target);
  err_check_propagate(retval);
  retval = eonce_load_TX_RX_to_r0(target);
  err_check_propagate(retval);
  retval = eonce_move_y0_at_r0(target);
  err_check_propagate(retval);
  // at this point the data i want is at the reg eonce can read
  retval = eonce_rx_lower_data(target,data_read);
  err_check_propagate(retval);
  LOG_DEBUG("%s: Data read from 0x%06X: 0x%04X",__FUNCTION__, address,*data_read);
  return retval;
}

static int dsp5680xx_read_32_single(struct target * target, uint32_t address, uint32_t * data_read, int r_pmem){
  int retval;
  address = (address & 0xFFFFFE);
  // Get data to an intermediate register
  retval = eonce_move_long_to_r0(target,address);
  err_check_propagate(retval);
  if(r_pmem){
    retval = eonce_move_at_pr0_inc_to_y0(target);
	err_check_propagate(retval);
    retval = eonce_move_at_pr0_inc_to_y1(target);
	err_check_propagate(retval);
  }else{
    retval = eonce_move_at_r0_inc_to_y0(target);
	err_check_propagate(retval);
    retval = eonce_move_at_r0_to_y1(target);
	err_check_propagate(retval);
  }
  // Get lower part of data to TX/RX
  retval = eonce_load_TX_RX_to_r0(target);
  err_check_propagate(retval);
  retval = eonce_move_y0_at_r0_inc(target); // This also load TX/RX high to r0
  err_check_propagate(retval);
  // Get upper part of data to TX/RX
  retval = eonce_move_y1_at_r0(target);
  err_check_propagate(retval);
  // at this point the data i want is at the reg eonce can read
  retval = eonce_rx_lower_data(target,(uint16_t * )data_read);
  err_check_propagate(retval);
  uint16_t tmp;
  retval = eonce_rx_upper_data(target,&tmp);
  err_check_propagate(retval);
  *data_read = ((tmp<<16) | (*data_read));//This enables opencd crc to succeed, even though it's very slow.
  return retval;
}

static int dsp5680xx_read(struct target * target, uint32_t address, unsigned size, unsigned count, uint8_t * buffer){
  if(target->state != TARGET_HALTED){
    LOG_USER("Target must be halted.");
    return ERROR_OK;
  }
  uint32_t * buff32 = (uint32_t *) buffer;
  uint16_t * buff16 = (uint16_t *) buffer;
  int retval = ERROR_OK;
  int pmem = 1;
  uint16_t tmp_wrd;

  retval = dsp5680xx_convert_address(&address, &pmem);
  err_check_propagate(retval);

  for (unsigned i=0; i<count; i++){
    switch (size){
    case 1:
      if(!(i%2)){
		retval = dsp5680xx_read_16_single(target, address + i/2, &tmp_wrd, pmem);
		buffer[i] = (uint8_t) (tmp_wrd>>8);
		buffer[i+1] = (uint8_t) (tmp_wrd&0xff);
      }
      break;
    case 2:
      retval = dsp5680xx_read_16_single(target, address + i, buff16 + i, pmem);
      break;
    case 4:
      retval = dsp5680xx_read_32_single(target, address + 2*i, buff32 + i, pmem);
      break;
    default:
      LOG_USER("%s: Invalid read size.",__FUNCTION__);
      break;
    }
	err_check_propagate(retval);
  }
  return retval;
}

//TODO doxy
static int dsp5680xx_write_16_single(struct target *target, uint32_t address, uint16_t data, uint8_t w_pmem){
  int retval = 0;
  retval = eonce_move_long_to_r0(target,address);
  err_check_propagate(retval);
  if(w_pmem){
    retval = eonce_move_value_to_y0(target,data);
	err_check_propagate(retval);
    retval = eonce_move_y0_at_pr0_inc(target);
	err_check_propagate(retval);
  }else{
    retval = eonce_move_value_at_r0(target,data);
	err_check_propagate(retval);
  }
  return retval;
}

//TODO doxy
static int dsp5680xx_write_32_single(struct target *target, uint32_t address, uint32_t data, int w_pmem){
  int retval = 0;
  retval = eonce_move_long_to_r0(target,address);
  err_check_propagate(retval);
  retval = eonce_move_long_to_y(target,data);
  err_check_propagate(retval);
  if(w_pmem)
    retval = eonce_move_y0_at_pr0_inc(target);
  else
    retval = eonce_move_y0_at_r0_inc(target);
  err_check_propagate(retval);
  if(w_pmem)
    retval = eonce_move_y1_at_pr0_inc(target);
  else
    retval = eonce_move_y1_at_r0_inc(target);
  err_check_propagate(retval);
  return retval;
}

static int dsp5680xx_write_8(struct target * target, uint32_t address, uint32_t count, uint8_t * data, int pmem){
  if(target->state != TARGET_HALTED){
    LOG_ERROR("%s: Target must be halted.",__FUNCTION__);
    return ERROR_OK;
  };
  int retval = 0;
  uint16_t * data_w = (uint16_t *)data;
  uint32_t iter;

  int counter = FLUSH_COUNT_WRITE;
  for(iter = 0; iter<count/2; iter++){
    if(--counter==0){
      context.flush = 1;
      counter = FLUSH_COUNT_WRITE;
    }
    retval = dsp5680xx_write_16_single(target,address+iter,data_w[iter], pmem);
    if(retval != ERROR_OK){
      LOG_ERROR("%s: Could not write to p:0x%04X",__FUNCTION__,address);
      context.flush = 1;
      return retval;
    }
    context.flush = 0;
  }
  context.flush = 1;

  // Only one byte left, let's not overwrite the other byte (mem is 16bit)
  // Need to retrieve the part we do not want to overwrite.
  uint16_t data_old;
  if((count==1)||(count%2)){
    retval = dsp5680xx_read(target,address+iter,1,1,(uint8_t *)&data_old);
	err_check_propagate(retval);
    if(count==1)
      data_old=(((data_old&0xff)<<8)|data[0]);// preserve upper byte
    else
      data_old=(((data_old&0xff)<<8)|data[2*iter+1]);
    retval = dsp5680xx_write_16_single(target,address+iter,data_old, pmem);
	err_check_propagate(retval);
  }
  return retval;
}

static int dsp5680xx_write_16(struct target * target, uint32_t address, uint32_t count, uint16_t * data, int pmem){
  int retval = ERROR_OK;
  if(target->state != TARGET_HALTED){
	retval = ERROR_TARGET_NOT_HALTED;
	err_check(retval,"Target must be halted.");
  };
  uint32_t iter;

  int counter_reset = FLUSH_COUNT_WRITE;
  int counter = counter_reset;

  for(iter = 0; iter<count; iter++){
	if(--counter==0){
	  context.flush = 1;
	  counter = counter_reset;
	}
    retval = dsp5680xx_write_16_single(target,address+iter,data[iter], pmem);
    if(retval != ERROR_OK){
      LOG_ERROR("%s: Could not write to p:0x%04X",__FUNCTION__,address);
	  context.flush = 1;
      return retval;
    }
	context.flush = 0;
  }
  context.flush = 1;
  return retval;
}

static int dsp5680xx_write_32(struct target * target, uint32_t address, uint32_t count, uint32_t * data, int pmem){
  int retval = ERROR_OK;
  if(target->state != TARGET_HALTED){
	retval = ERROR_TARGET_NOT_HALTED;
	err_check(retval,"Target must be halted.");
  };
  uint32_t iter;

  int counter_reset = FLUSH_COUNT_WRITE;
  int counter = counter_reset;

  for(iter = 0; iter<count; iter++){
	if(--counter==0){
	  context.flush = 1;
	  counter = counter_reset;
	}
    retval = dsp5680xx_write_32_single(target,address+(iter<<1),data[iter], pmem);
    if(retval != ERROR_OK){
      LOG_ERROR("%s: Could not write to p:0x%04X",__FUNCTION__,address);
	  context.flush = 1;
      return retval;
    }
	context.flush = 0;
  }
  context.flush = 1;
  return retval;
}

//TODO doxy
static int dsp5680xx_write(struct target *target, uint32_t address, uint32_t size, uint32_t count, const uint8_t * buffer){
  //TODO Cannot write 32bit to odd address, will write 0x12345678  as 0x5678 0x0012
  if(target->state != TARGET_HALTED){
    LOG_USER("Target must be halted.");
    return ERROR_OK;
  }
  int retval = 0;
  int p_mem = 1;
  retval = dsp5680xx_convert_address(&address, &p_mem);
  err_check_propagate(retval);

  switch (size){
  case 1:
    retval = dsp5680xx_write_8(target, address, count,(uint8_t *) buffer, p_mem);
    break;
  case 2:
    retval = dsp5680xx_write_16(target, address, count, (uint16_t *)buffer, p_mem);
      break;
  case 4:
    retval = dsp5680xx_write_32(target, address, count, (uint32_t *)buffer, p_mem);
    break;
  default:
	retval = ERROR_TARGET_DATA_ABORT;
	err_check(retval,"Invalid data size.")
	break;
  }
  return retval;
}

static int dsp5680xx_bulk_write_memory(struct target * target,uint32_t address, uint32_t aligned, const uint8_t * buffer){
  LOG_ERROR("Not implemented yet.");
  return ERROR_FAIL;
}

// Writes to pram at address
// r3 holds the destination address-> p:(r3)
// r2 hold 0xf151 to flash a led (probably cannot see it due to high freq.)
// r0 holds TX/RX address.
//0x00000073  0x8A44FFFE017B         brclr       #1,X:(R0-2),*-2
//0x00000076  0xE700                 nop
//0x00000077  0xF514                 move.w      X:(R0),Y0
//0x00000078  0xE700                 nop
//0x00000079  0x8563                 move.w      Y0,P:(R3)+
//0x0000007A  0x84420003             bfchg       #3,X:(R2)
//0x0000007C  0xA976                 bra         *-9
uint16_t pgm_write_pram[] = {0x8A44,0xFFFE,0x017D,0xE700,0xF514,0xE700,0x8563,0x8442,0x0003,0xA976};
uint16_t pgm_write_pram_length = 10;

static int dsp5680xx_write_buffer(struct target * target, uint32_t address, uint32_t size, const uint8_t * buffer){
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  // this solution works, but it's slow. it flushes USB all the time.
  return dsp5680xx_write(target, address, 1, size, buffer);
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
}

static int dsp5680xx_read_buffer(struct target * target, uint32_t address, uint32_t size, uint8_t * buffer){
  if(target->state != TARGET_HALTED){
    LOG_USER("Target must be halted.");
    return ERROR_OK;
  }
  // byte addressing!
  int retval = ERROR_OK;
  int pmem = 1;
  uint16_t tmp_wrd= 0;

  retval = dsp5680xx_convert_address(&address, &pmem);
  err_check_propagate(retval);

  for (unsigned i=0; i<size; i++)
    if(!(i%2)){
      retval = dsp5680xx_read_16_single(target, address + i/2, &tmp_wrd, pmem);
	  err_check_propagate(retval);
      //TODO find a better solution. endiannes differs from normal read, otherwise the openocd crc would do weird stuff.
      buffer[i+1] = (uint8_t) (tmp_wrd>>8);
      buffer[i] = (uint8_t) (tmp_wrd&0xff);
   }
  return retval;
}

static int dsp5680xx_checksum_memory(struct target * target, uint32_t address, uint32_t size, uint32_t * checksum){
 //TODO implement.
  //This will make openocd do the work, but it will fail because of the word/byte addressing issues.
  int retval;
  struct working_area * crc_algorithm;
  retval = target_alloc_working_area(target, 20, &crc_algorithm);
  if(retval != ERROR_OK)
    return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
  retval = target_free_working_area(target, crc_algorithm);
  return ERROR_FAIL;
}

// Data signature algorithm used by the core FM (flash module)
static int perl_crc(uint16_t * buff16,uint32_t  word_count){
  uint16_t checksum = 0xffff;
  uint16_t data,fbmisr;
  uint32_t i;
  for(i=0;i<word_count;i++){
    data = buff16[i];
    fbmisr = (checksum & 2)>>1 ^ (checksum & 4)>>2 ^ (checksum & 16)>>4 ^ (checksum & 0x8000)>>15;
    checksum = (data ^ ((checksum << 1) | fbmisr));
  }
  i--;
  for(;!(i&0x80000000);i--){
    data = buff16[i];
    fbmisr = (checksum & 2)>>1 ^ (checksum & 4)>>2 ^ (checksum & 16)>>4 ^ (checksum & 0x8000)>>15;
    checksum = (data ^ ((checksum << 1) | fbmisr));
  }
  return checksum;
}

int dsp5680xx_f_SIM_reset(struct target * target){
  int retval = ERROR_OK;
  uint16_t sim_cmd = SIM_CMD_RESET;
  uint32_t sim_addr;
  if(strcmp(target->tap->chip,"dsp568013")==0){
	sim_addr = MC568013_SIM_BASE_ADDR+S_FILE_DATA_OFFSET;
	retval = dsp5680xx_write(target,sim_addr,1,2,(const uint8_t *)&sim_cmd);
	err_check_propagate(retval);
  }
  return retval;
}

//TODO doxy
static int dsp5680xx_soft_reset_halt(struct target *target){
  //TODO is this what this function is expected to do...?
  int retval;
  retval = dsp5680xx_halt(target);
  err_check_propagate(retval);
  retval = dsp5680xx_f_SIM_reset(target);
  err_check_propagate(retval);
  return retval;
}

int dsp5680xx_f_protect_check(struct target * target, uint8_t * protected) {
  uint16_t i,j;
  int retval;
  if (dsp5680xx_target_status(target,NULL,NULL) != TARGET_HALTED){
    retval = dsp5680xx_halt(target);
	err_check_propagate(retval);
  }
  retval = eonce_load_TX_RX_high_to_r0(target);
  err_check_propagate(retval);
  retval = eonce_move_value_to_y0(target,0x1234);
  err_check_propagate(retval);
  retval = eonce_move_y0_at_r0(target);
  err_check_propagate(retval);
  retval = eonce_rx_upper_data(target,&i);
  err_check_propagate(retval);
  retval = eonce_move_value_to_y0(target,0x4321);
  err_check_propagate(retval);
  retval = eonce_move_y0_at_r0(target);
  err_check_propagate(retval);
  retval = eonce_rx_upper_data(target,&j);
  err_check_propagate(retval);
  if(protected!=NULL)
    *protected = (uint8_t) ((i!=0x1234)||(j!=0x4321));
  return retval;
}

static int dsp5680xx_f_execute_command(struct target * target, uint16_t command, uint32_t address, uint32_t data, uint16_t * hfm_ustat, int pmem){
  int retval;
  retval = eonce_load_TX_RX_high_to_r0(target);
  err_check_propagate(retval);
  retval = eonce_move_long_to_r2(target,HFM_BASE_ADDR);
  err_check_propagate(retval);
  uint16_t i;
  int watchdog = 100;
  do{
    retval = eonce_move_at_r2_disp_to_y0(target,HFM_USTAT);	// read HMF_USTAT
	err_check_propagate(retval);
    retval = eonce_move_y0_at_r0(target);
	err_check_propagate(retval);
    retval = eonce_rx_upper_data(target,&i);
	err_check_propagate(retval);
    if((watchdog--)==1){
      retval = ERROR_TARGET_FAILURE;
      err_check(retval,"FM execute command failed.");
    }
  }while (!(i&0x40));				// wait until current command is complete
  retval = eonce_move_value_at_r2_disp(target,0x00,HFM_CNFG);	// write to HFM_CNFG (lock=0, select bank) -- flash_desc.bank&0x03,0x01 == 0x00,0x01 ???
  err_check_propagate(retval);
  retval = eonce_move_value_at_r2_disp(target,0x04,HFM_USTAT);		// write to HMF_USTAT, clear PVIOL, ACCERR & BLANK bits
  err_check_propagate(retval);
  retval = eonce_move_value_at_r2_disp(target,0x10,HFM_USTAT);		// clear only one bit at a time
  err_check_propagate(retval);
  retval = eonce_move_value_at_r2_disp(target,0x20,HFM_USTAT);
  err_check_propagate(retval);
  retval = eonce_move_value_at_r2_disp(target,0x00,HFM_PROT);		// write to HMF_PROT, clear protection
  err_check_propagate(retval);
  retval = eonce_move_value_at_r2_disp(target,0x00,HFM_PROTB);		// write to HMF_PROTB, clear protection
  err_check_propagate(retval);
  retval = eonce_move_value_to_y0(target,data);
  err_check_propagate(retval);
  retval = eonce_move_long_to_r3(target,address);			// write to the flash block
  err_check_propagate(retval);
  if (pmem){
    retval = eonce_move_y0_at_pr3_inc(target);
	err_check_propagate(retval);
  }else{
    retval = eonce_move_y0_at_r3(target);
	err_check_propagate(retval);
  }
  retval = eonce_move_value_at_r2_disp(target,command,HFM_CMD);	// write command to the HFM_CMD reg
  err_check_propagate(retval);
  retval = eonce_move_value_at_r2_disp(target,0x80,HFM_USTAT);		// start the command
  err_check_propagate(retval);
  watchdog = 100;
  do{
    retval = eonce_move_at_r2_disp_to_y0(target,HFM_USTAT);	// read HMF_USTAT
	err_check_propagate(retval);
    retval = eonce_move_y0_at_r0(target);
	err_check_propagate(retval);
	retval = eonce_rx_upper_data(target,&i);
	err_check_propagate(retval);
    if((watchdog--)==1){
	  retval = ERROR_TARGET_FAILURE;
      err_check(retval,"FM execution did not finish.");
    }
  }while (!(i&0x40));	    // wait until the command is complete
  *hfm_ustat = i;
  return ERROR_OK;
}

static int eonce_set_hfmdiv(struct target * target){
  uint16_t i;
  int retval;
  retval = eonce_move_long_to_r2(target,HFM_BASE_ADDR);
  err_check_propagate(retval);
  retval = eonce_load_TX_RX_high_to_r0(target);
  err_check_propagate(retval);
  retval = eonce_move_at_r2_to_y0(target);// read HFM_CLKD
  err_check_propagate(retval);
  retval = eonce_move_y0_at_r0(target);
  err_check_propagate(retval);
  retval = eonce_rx_upper_data(target,&i);
  err_check_propagate(retval);
  unsigned int hfm_at_wrong_value = 0;
  if ((i&0x7f)!=HFM_CLK_DEFAULT) {
    LOG_DEBUG("HFM CLK divisor contained incorrect value (0x%02X).",i&0x7f);
    hfm_at_wrong_value = 1;
  }else{
    LOG_DEBUG("HFM CLK divisor was already set to correct value (0x%02X).",i&0x7f);
    return ERROR_OK;
  }
  retval = eonce_move_value_at_r2(target,HFM_CLK_DEFAULT);	// write HFM_CLKD
  err_check_propagate(retval);
  retval = eonce_move_at_r2_to_y0(target); // verify HFM_CLKD
  err_check_propagate(retval);
  retval = eonce_move_y0_at_r0(target);
  err_check_propagate(retval);
  retval = eonce_rx_upper_data(target,&i);
  err_check_propagate(retval);
  if (i!=(0x80|(HFM_CLK_DEFAULT&0x7f))) {
	retval = ERROR_TARGET_FAILURE;
	err_check(retval,"Unable to set HFM CLK divisor.");
  }
  if(hfm_at_wrong_value)
    LOG_DEBUG("HFM CLK divisor set to 0x%02x.",i&0x7f);
  return ERROR_OK;
}

static int dsp5680xx_f_signature(struct target * target, uint32_t address, uint32_t words, uint16_t * signature){
  int retval;
  uint16_t hfm_ustat;
  if (dsp5680xx_target_status(target,NULL,NULL) != TARGET_HALTED){
    retval = eonce_enter_debug_mode(target,NULL);
    err_check_propagate(retval);
  }
  retval = dsp5680xx_f_execute_command(target,HFM_CALCULATE_DATA_SIGNATURE,address,words,&hfm_ustat,1);
  err_check_propagate(retval);
  if (hfm_ustat&HFM_USTAT_MASK_PVIOL_ACCER){
    retval = ERROR_TARGET_FAILURE;
    err_check(retval,"HFM exec error:pviol and/or accer bits set.");
  }
  retval = dsp5680xx_read_16_single(target, HFM_BASE_ADDR|HFM_DATA, signature, 0);
  return retval;
}

int dsp5680xx_f_erase_check(struct target * target, uint8_t * erased){
  int retval;
  uint16_t hfm_ustat;
  if (dsp5680xx_target_status(target,NULL,NULL) != TARGET_HALTED){
    retval = dsp5680xx_halt(target);
    err_check_propagate(retval);
  }
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  // Check security
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  uint8_t protected;
  retval = dsp5680xx_f_protect_check(target,&protected);
  err_check_propagate(retval);
  if(protected){
	retval = ERROR_TARGET_FAILURE;
	err_check(retval,"Failed to erase, flash is still protected.");
  }
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  // Set hfmdiv
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  retval = eonce_set_hfmdiv(target);
  err_check_propagate(retval);

  // Check if chip is already erased.
  // Since only mass erase is currently implemented, only the first sector is checked (assuming no code will leave it unused)
  retval = dsp5680xx_f_execute_command(target,HFM_ERASE_VERIFY,HFM_FLASH_BASE_ADDR+0*HFM_SECTOR_SIZE,0,&hfm_ustat,1); // blank check
  err_check_propagate(retval);
  if (hfm_ustat&HFM_USTAT_MASK_PVIOL_ACCER){
	retval = ERROR_TARGET_FAILURE;
	err_check(retval,"pviol and/or accer bits set. EraseVerify HFM command execution error");;
  }
  if(erased!=NULL)
    *erased = (uint8_t)(hfm_ustat&HFM_USTAT_MASK_BLANK);
  return retval;
}

int dsp5680xx_f_erase(struct target * target, int first, int last){
  //TODO implement erasing individual sectors.
  int retval;
  if(first||last){
	retval = ERROR_FAIL;
	err_check(retval,"Sector erasing not implemented. Call with first=last=0.");
  }
  if (dsp5680xx_target_status(target,NULL,NULL) != TARGET_HALTED){
    retval = dsp5680xx_halt(target);
	err_check_propagate(retval);
  }
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  // Reset SIM
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  retval = dsp5680xx_f_SIM_reset(target);
  err_check_propagate(retval);
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  // Check security
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  uint8_t protected;
  retval = dsp5680xx_f_protect_check(target,&protected);
  err_check_propagate(retval);
  if(protected){
	retval = ERROR_TARGET_FAILURE;
	err_check(retval,"Cannot flash, security is still enabled.");
  }
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  // Set hfmdiv
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  retval = eonce_set_hfmdiv(target);
  err_check_propagate(retval);

  // Check if chip is already erased.
  // Since only mass erase is currently implemented, only the first sector is checked (assuming no code will leave it unused)
  uint8_t erased;
  retval = dsp5680xx_f_erase_check(target,&erased);
  err_check_propagate(retval);
  if (erased)
    LOG_USER("Flash blank - mass erase skipped.");
  else{
    // Execute mass erase command.
	uint16_t hfm_ustat;
	uint16_t hfm_cmd = HFM_MASS_ERASE;
	retval = dsp5680xx_f_execute_command(target,hfm_cmd,HFM_FLASH_BASE_ADDR+0*HFM_SECTOR_SIZE,0,&hfm_ustat,1);
	err_check_propagate(retval);
    if (hfm_ustat&HFM_USTAT_MASK_PVIOL_ACCER){
	  retval = ERROR_TARGET_FAILURE;
	  err_check(retval,"pviol and/or accer bits set. HFM command execution error");
    }
    // Verify flash was successfully erased.
    retval = dsp5680xx_f_erase_check(target,&erased);
	err_check_propagate(retval);
	if(retval == ERROR_OK){
      if (erased)
		LOG_USER("Flash mass erased and checked blank.");
      else
		LOG_WARNING("Flash mass erased, but still not blank!");
    }
  }
  return retval;
}

// Algorithm for programming normal p: flash
// Follow state machine from "56F801x Peripheral Reference Manual"@163.
// Registers to set up before calling:
//  r0: TX/RX high address.
//  r2: FM module base address.
//  r3: Destination address in flash.
//
//		hfm_wait:                                           // wait for command to finish
//			brclr	#0x40,x:(r2+0x13),hfm_wait
//		rx_check:					    // wait for input buffer full
//			brclr	#0x01,x:(r0-2),rx_check
//			move.w	x:(r0),y0		   	    // read from Rx buffer
//			move.w	y0,p:(r3)+
//			move.w	#0x20,x:(r2+0x14)		    // write PGM command
//			move.w	#0x80,x:(r2+0x13)		    // start the command
//                      brclr       #0x20,X:(R2+0x13),accerr_check  // protection violation check
//                      bfset       #0x20,X:(R2+0x13)               // clear pviol
//                      bra         hfm_wait
//              accerr_check:
//                      brclr       #0x10,X:(R2+0x13),hfm_wait      // access error check
//                      bfset       #0x10,X:(R2+0x13)               // clear accerr
//			bra	    hfm_wait		            // loop
//0x00000073  0x8A460013407D         brclr       #0x40,X:(R2+0x13),*+0
//0x00000076  0xE700                 nop
//0x00000077  0xE700                 nop
//0x00000078  0x8A44FFFE017B         brclr       #1,X:(R0-2),*-2
//0x0000007B  0xE700                 nop
//0x0000007C  0xF514                 move.w      X:(R0),Y0
//0x0000007D  0x8563                 move.w      Y0,P:(R3)+
//0x0000007E  0x864600200014         move.w      #0x20,X:(R2+0x14)
//0x00000081  0x864600800013         move.w      #0x80,X:(R2+0x13)
//0x00000084  0x8A4600132004         brclr       #0x20,X:(R2+0x13),*+7
//0x00000087  0x824600130020         bfset       #0x20,X:(R2+0x13)
//0x0000008A  0xA968                 bra         *-23
//0x0000008B  0x8A4600131065         brclr       #0x10,X:(R2+0x13),*-24
//0x0000008E  0x824600130010         bfset       #0x10,X:(R2+0x13)
//0x00000091  0xA961                 bra         *-30
const uint16_t pgm_write_pflash[] = {0x8A46,0x0013,0x407D,0xE700,0xE700,0x8A44,0xFFFE,0x017B,0xE700,0xF514,0x8563,0x8646,0x0020,0x0014,0x8646,0x0080,0x0013,0x8A46,0x0013,0x2004,0x8246,0x0013,0x0020,0xA968,0x8A46,0x0013,0x1065,0x8246,0x0013,0x0010,0xA961};
const uint32_t pgm_write_pflash_length = 31;

int dsp5680xx_f_wr(struct target * target, uint8_t *buffer, uint32_t address, uint32_t count){
  int retval = ERROR_OK;
  uint16_t* buff16 = (uint16_t *) buffer;
  if (dsp5680xx_target_status(target,NULL,NULL) != TARGET_HALTED){
    retval = dsp5680xx_halt(target);
	err_check_propagate(retval);
  }
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  // Check if flash is erased
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  uint8_t erased;
  retval = dsp5680xx_f_erase_check(target,&erased);
  err_check_propagate(retval);
  if(!erased){
	retval = ERROR_FAIL;
	err_check(retval,"Flash must be erased before flashing.");
  }
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  // Download the pgm that flashes.
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  uint32_t my_favourite_ram_address = 0x8700; // This seems to be a safe address. This one is the one used by codewarrior in 56801x_flash.cfg
  retval = dsp5680xx_write(target, my_favourite_ram_address, 1, pgm_write_pflash_length*2,(uint8_t *) pgm_write_pflash);
  err_check_propagate(retval);
  retval = dsp5680xx_execute_queue();
  err_check_propagate(retval);
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  // Set hfmdiv
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  retval = eonce_set_hfmdiv(target);
  err_check_propagate(retval);
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  // Setup registers needed by pgm_write_pflash
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  retval = eonce_move_long_to_r3(target,address);  // Destination address to r3
  err_check_propagate(retval);
  eonce_load_TX_RX_high_to_r0(target);  // TX/RX reg address to r0
  err_check_propagate(retval);
  retval = eonce_move_long_to_r2(target,HFM_BASE_ADDR);// FM base address to r2
  err_check_propagate(retval);
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  // Run flashing program.
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  retval = eonce_move_value_at_r2_disp(target,0x00,HFM_CNFG); // write to HFM_CNFG (lock=0, select bank)
  err_check_propagate(retval);
  retval = eonce_move_value_at_r2_disp(target,0x04,HFM_USTAT);// write to HMF_USTAT, clear PVIOL, ACCERR & BLANK bits
  err_check_propagate(retval);
  retval = eonce_move_value_at_r2_disp(target,0x10,HFM_USTAT);// clear only one bit at a time
  err_check_propagate(retval);
  retval = eonce_move_value_at_r2_disp(target,0x20,HFM_USTAT);
  err_check_propagate(retval);
  retval = eonce_move_value_at_r2_disp(target,0x00,HFM_PROT);// write to HMF_PROT, clear protection
  err_check_propagate(retval);
  retval = eonce_move_value_at_r2_disp(target,0x00,HFM_PROTB);// write to HMF_PROTB, clear protection
  err_check_propagate(retval);
  if(count%2){
    //TODO implement handling of odd number of words.
	retval = ERROR_FAIL;
	err_check(retval,"Cannot handle odd number of words.");
  }
  uint32_t drscan_data;
  retval = eonce_tx_upper_data(target,buff16[0],&drscan_data);
  err_check_propagate(retval);

  retval = dsp5680xx_resume(target,0,my_favourite_ram_address,0,0);
  err_check_propagate(retval);

  int counter_reset = FLUSH_COUNT_FLASH;
  int counter = counter_reset;
  context.flush = 0;
  uint32_t i;
  for(i=1; (i<count/2)&&(i<HFM_SIZE_REAL); i++){
    if(--counter==0){
      context.flush = 1;
      counter = counter_reset;
    }
    retval = eonce_tx_upper_data(target,buff16[i],&drscan_data);
	if(retval!=ERROR_OK){
	  context.flush = 1;
	  err_check_propagate(retval);
	}
	context.flush = 0;
  }
  context.flush = 1;
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  // Verify flash
  // -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
  uint16_t signature;
  uint16_t pc_crc;
  retval =  dsp5680xx_f_signature(target,address,i,&signature);
  err_check_propagate(retval);
  pc_crc = perl_crc(buff16,i);
  if(pc_crc != signature){
    retval = ERROR_FAIL;
    err_check(retval,"Flashed data failed CRC check, flash again!");
  }
  return retval;
}



int dsp5680xx_f_unlock(struct target * target){
  int retval;
  if(target->tap->enabled){
    //TODO find a way to switch to the master tap here.
    LOG_ERROR("Master tap must be enabled to unlock flash.");
    return ERROR_TARGET_FAILURE;
  }
  uint32_t data_to_shift_in = MASTER_TAP_CMD_FLASH_ERASE;
  uint32_t data_shifted_out;
  retval = dsp5680xx_irscan(target,&data_to_shift_in,&data_shifted_out,8);
  err_check_propagate(retval);
  data_to_shift_in = HFM_CLK_DEFAULT;
  retval = dsp5680xx_drscan(target,((uint8_t *) & data_to_shift_in),((uint8_t *)&data_shifted_out),8);
  err_check_propagate(retval);
  return retval;
}

int dsp5680xx_f_lock(struct target * target){
  int retval;
  uint16_t lock_word[] = {HFM_LOCK_FLASH,HFM_LOCK_FLASH};
  retval = dsp5680xx_f_wr(target,(uint8_t *)(lock_word),HFM_LOCK_ADDR_L,4);
  err_check_propagate(retval);
  return retval;
}

static int dsp5680xx_step(struct target * target,int current, uint32_t address, int handle_breakpoints){
  err_check(ERROR_FAIL,"Not implemented yet.");
}

/** Holds methods for dsp5680xx targets. */
struct target_type dsp5680xx_target = {
  .name = "dsp5680xx",

  .poll = dsp5680xx_poll,
  .arch_state = dsp5680xx_arch_state,

  .target_request_data = NULL,

  .halt = dsp5680xx_halt,
  .resume = dsp5680xx_resume,
  .step = dsp5680xx_step,

  .write_buffer = dsp5680xx_write_buffer,
  .read_buffer = dsp5680xx_read_buffer,

  .assert_reset = dsp5680xx_assert_reset,
  .deassert_reset = dsp5680xx_deassert_reset,
  .soft_reset_halt = dsp5680xx_soft_reset_halt,

  .read_memory = dsp5680xx_read,
  .write_memory = dsp5680xx_write,
  .bulk_write_memory = dsp5680xx_bulk_write_memory,

  .checksum_memory = dsp5680xx_checksum_memory,

  .target_create = dsp5680xx_target_create,
  .init_target = dsp5680xx_init_target,
};
