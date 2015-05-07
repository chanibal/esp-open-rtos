/*
    FreeRTOS V7.5.2 - Copyright (C) 2013 Real Time Engineers Ltd.

    VISIT http://www.FreeRTOS.org TO ENSURE YOU ARE USING THE LATEST VERSION.

    ***************************************************************************
     *                                                                       *
     *    FreeRTOS provides completely free yet professionally developed,    *
     *    robust, strictly quality controlled, supported, and cross          *
     *    platform software that has become a de facto standard.             *
     *                                                                       *
     *    Help yourself get started quickly and support the FreeRTOS         *
     *    project by purchasing a FreeRTOS tutorial book, reference          *
     *    manual, or both from: http://www.FreeRTOS.org/Documentation        *
     *                                                                       *
     *    Thank you!                                                         *
     *                                                                       *
    ***************************************************************************

    This file is part of the FreeRTOS distribution.

    FreeRTOS is free software; you can redistribute it and/or modify it under
    the terms of the GNU General Public License (version 2) as published by the
    Free Software Foundation >>!AND MODIFIED BY!<< the FreeRTOS exception.

    >>! NOTE: The modification to the GPL is included to allow you to distribute
    >>! a combined work that includes FreeRTOS without being obliged to provide
    >>! the source code for proprietary components outside of the FreeRTOS
    >>! kernel.

    FreeRTOS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE.  Full license text is available from the following
    link: http://www.freertos.org/a00114.html

    1 tab == 4 spaces!

    ***************************************************************************
     *                                                                       *
     *    Having a problem?  Start by reading the FAQ "My application does   *
     *    not run, what could be wrong?"                                     *
     *                                                                       *
     *    http://www.FreeRTOS.org/FAQHelp.html                               *
     *                                                                       *
    ***************************************************************************

    http://www.FreeRTOS.org - Documentation, books, training, latest versions,
    license and Real Time Engineers Ltd. contact details.

    http://www.FreeRTOS.org/plus - A selection of FreeRTOS ecosystem products,
    including FreeRTOS+Trace - an indispensable productivity tool, a DOS
    compatible FAT file system, and our tiny thread aware UDP/IP stack.

    http://www.OpenRTOS.com - Real Time Engineers ltd license FreeRTOS to High
    Integrity Systems to sell under the OpenRTOS brand.  Low cost OpenRTOS
    licenses offer ticketed support, indemnification and middleware.

    http://www.SafeRTOS.com - High Integrity Systems also provide a safety
    engineered and independently SIL3 certified version for use in safety and
    mission critical applications that require provable dependability.

    1 tab == 4 spaces!
*/

/*-----------------------------------------------------------
 * Implementation of functions defined in portable.h for ESP8266
 *
 * This is based on the version supplied in esp_iot_rtos_sdk,
 * which is in turn based on the ARM CM3 port.
 *----------------------------------------------------------*/

/* Scheduler includes. */
#include <xtensa/config/core.h>

#include "FreeRTOS.h"
#include "task.h"
#include "xtensa_rtos.h"

unsigned cpu_sr;
char level1_int_disabled;

/*
 * Stack initialization
 */
portSTACK_TYPE *pxPortInitialiseStack( portSTACK_TYPE *pxTopOfStack, pdTASK_CODE pxCode, void *pvParameters )
{
    #define SET_STKREG(r,v) sp[(r) >> 2] = (portSTACK_TYPE)(v)
    portSTACK_TYPE *sp, *tp;

    /* Create interrupt stack frame aligned to 16 byte boundary */
    sp = (portSTACK_TYPE*) (((uint32_t)(pxTopOfStack+1) - XT_CP_SIZE - XT_STK_FRMSZ) & ~0xf);

    /* Clear the entire frame (do not use memset() because we don't depend on C library) */
    for (tp = sp; tp <= pxTopOfStack; ++tp)
        *tp = 0;

    /* Explicitly initialize certain saved registers */
    SET_STKREG( XT_STK_PC,      pxCode                        );  /* task entrypoint                  */
    SET_STKREG( XT_STK_A0,      0                           );  /* to terminate GDB backtrace       */
    SET_STKREG( XT_STK_A1,      (uint32_t)sp + XT_STK_FRMSZ   );  /* physical top of stack frame      */
    SET_STKREG( XT_STK_A2,      pvParameters   );           /* parameters      */
    SET_STKREG( XT_STK_EXIT,    _xt_user_exit               );  /* user exception exit dispatcher   */

    /* Set initial PS to int level 0, EXCM disabled ('rfe' will enable), user mode. */
    SET_STKREG( XT_STK_PS,      PS_UM | PS_EXCM     );
    return sp;
}

static int pending_soft_sv;
static int pending_maclayer_sv;

/* PendSV is called in place of vPortYield() to request a supervisor
   call.

   The portYIELD macro calls pendSV if it's a software request.

   The libpp and libudhcp libraries also call this function, assuming
   always with arg==2 (but maybe sometimes with arg==1?)

   In the original esp_iot_rtos_sdk implementation, arg was a char. Using an
   enum is ABI-compatible, though.
*/
void PendSV(enum SVC_ReqType req)
{
	vPortEnterCritical();

	if(req == SVC_Software)
	{
		pending_soft_sv = 1;
	}
	else if(req == SVC_MACLayer)
		pending_maclayer_sv= 1;

	xthal_set_intset(1<<ETS_SOFT_INUM);
	vPortExitCritical();
}

/* This MAC layer ISR handler is defined in libpp.a, and is called
 * after a Blob SV requests a soft interrupt by calling
 * PendSV(SVC_MACLayer).
 */
extern portBASE_TYPE MacIsrSigPostDefHdl(void);

void SV_ISR(void)
{
	portBASE_TYPE xHigherPriorityTaskWoken=pdFALSE ;
	if(pending_maclayer_sv)
	{
		xHigherPriorityTaskWoken = MacIsrSigPostDefHdl();
		pending_maclayer_sv = 0;
	}
	if( xHigherPriorityTaskWoken || pending_soft_sv)
	{
	    _xt_timer_int1();
	    pending_soft_sv = 0;
	}
}

void xPortSysTickHandle (void)
{
	//CloseNMI();
	{
		if(xTaskIncrementTick() !=pdFALSE )
		{
			//GPIO_REG_WRITE(GPIO_STATUS_W1TS_ADDRESS, 0x40);
			vTaskSwitchContext();
		}
	}
	//OpenNMI();
}

/*
 * See header file for description.
 */
portBASE_TYPE xPortStartScheduler( void )
{
    _xt_isr_attach(ETS_SOFT_INUM, SV_ISR);
    _xt_isr_unmask(1<<ETS_SOFT_INUM);

    /* Initialize system tick timer interrupt and schedule the first tick. */
    _xt_tick_timer_init();

    vTaskSwitchContext();

    _xt_int_exit();

    /* Should not get here as the tasks are now running! */
    return pdTRUE;
}

void vPortEndScheduler( void )
{
    /* No-op, nothing to return to */
}

/*-----------------------------------------------------------*/

/* Each task maintains its own interrupt status in the critical nesting
variable. */
static unsigned portBASE_TYPE uxCriticalNesting = 0;

void vPortEnterCritical( void )
{
    portDISABLE_INTERRUPTS();
    uxCriticalNesting++;
}
/*-----------------------------------------------------------*/

void vPortExitCritical( void )
{
    uxCriticalNesting--;
    if( uxCriticalNesting == 0 )
	portENABLE_INTERRUPTS();
}

/*-----------------------------------------------------------*/

/* Main ISR handler for FreeRTOS side of the ESP libs?

   As far as I can tell, the "real" Xtensa ISRs ("Exceptions") are
   handled in libmain.a (xtensa_vectors.o) which then can call into here
   passing an interrupt mask.
*/

_xt_isr isr[16];

void _xt_isr_attach(uint8_t i, _xt_isr func)
{
    isr[i] = func;
}

uint16_t _xt_isr_handler(uint16_t i)
{
    uint8_t index;

    if (i & (1 << ETS_WDT_INUM)) {
	index = ETS_WDT_INUM;
    }
    else if (i & (1 << ETS_GPIO_INUM)) {
	index = ETS_GPIO_INUM;
    }else {
	index = __builtin_ffs(i) - 1;

	if (index == ETS_MAX_INUM) {
	    i &= ~(1 << ETS_MAX_INUM);
	    index = __builtin_ffs(i) - 1;
	}
    }

    _xt_clear_ints(1<<index);

    isr[index]();

    return i & ~(1 << index);
}