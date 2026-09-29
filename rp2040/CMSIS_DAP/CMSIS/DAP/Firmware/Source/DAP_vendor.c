/*
 * Copyright (c) 2013-2017 ARM Limited. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * ----------------------------------------------------------------------
 *
 * $Date:        1. December 2017
 * $Revision:    V2.0.0
 *
 * Project:      CMSIS-DAP Source
 * Title:        DAP_vendor.c CMSIS-DAP Vendor Commands
 *
 *---------------------------------------------------------------------------*/
 
#include "DAP_config.h"
#include "DAP.h"

// Custom vendor trigger
void cdc_activate_profile( int uartId, int profileId );


//**************************************************************************************************
/** 
\defgroup DAP_Vendor_Adapt_gr Adapt Vendor Commands
\ingroup DAP_Vendor_gr 
@{

The file DAP_vendor.c provides template source code for extension of a Debug Unit with 
Vendor Commands. Copy this file to the project folder of the Debug Unit and add the 
file to the MDK-ARM project under the file group Configuration.
*/

/** Process DAP Vendor Command and prepare Response Data
\param request   pointer to request data
\param response  pointer to response data
\return          number of bytes in response (lower 16 bits)
                 number of bytes in request (upper 16 bits)
*/
uint32_t DAP_ProcessVendorCommand(uint32_t probeId, const uint8_t *request, uint8_t *response) {
  uint32_t num = (1U << 16) | 1U;

  *response++ = *request;        // copy Command ID

  uint8_t cmd = *request++;
  switch (cmd) {          // first byte in request is Command ID
    case ID_DAP_Vendor0: break;      
    case ID_DAP_Vendor1:  
      cdc_activate_profile( 0, 0 );  // UART 1 -> ACM0
      break;    
    case ID_DAP_Vendor2:  
      cdc_activate_profile( 0, 1 );  // UART 2 -> ACM0
      break;
    case ID_DAP_Vendor3: 
      cdc_activate_profile( 0, 2 );  // UART 3 -> ACM0
      break;    
    case ID_DAP_Vendor4:
      cdc_activate_profile( 0, 3 );  // UART 4 -> ACM0
      break;          
    case ID_DAP_Vendor5:  break;
    case ID_DAP_Vendor6:  break;
    case ID_DAP_Vendor7:  break;
    case ID_DAP_Vendor8:  break;
    case ID_DAP_Vendor9:  break;
    case ID_DAP_Vendor10: break;
    case ID_DAP_Vendor11: break;
    case ID_DAP_Vendor12: break;
    case ID_DAP_Vendor13: break;
    case ID_DAP_Vendor14: break;
    case ID_DAP_Vendor15: break;
    case ID_DAP_Vendor16: break;
    case ID_DAP_Vendor17:
      cdc_activate_profile( 1, 0 );  // UART 1 -> ACM1
      break;    
    case ID_DAP_Vendor18:
      cdc_activate_profile( 1, 1 );  // UART 2 -> ACM1
      break;        
    case ID_DAP_Vendor19: 
      cdc_activate_profile( 1, 2 );  // UART 3 -> ACM1
      break;                  
    case ID_DAP_Vendor20: 
      cdc_activate_profile( 1, 3 );  // UART 4 -> ACM1
      break;            
    case ID_DAP_Vendor21: break;
    case ID_DAP_Vendor22: break;
    case ID_DAP_Vendor23: break;
    case ID_DAP_Vendor24: break;
    case ID_DAP_Vendor25: break;
    case ID_DAP_Vendor26: break;
    case ID_DAP_Vendor27: break;
    case ID_DAP_Vendor28: break;
    case ID_DAP_Vendor29: break;
    case ID_DAP_Vendor30: break;
    case ID_DAP_Vendor31: break;
  }

  return (num);
}

///@}
