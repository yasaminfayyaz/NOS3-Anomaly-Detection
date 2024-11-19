/************************************************************************
 * NASA Docket No. GSC-18,719-1, and identified as “core Flight System: Bootes”
 *
 * Copyright (c) 2020 United States Government as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ************************************************************************/

/**
 * @file
 *   This file is main hdr file for the Command Ingest lab application.
 */
#ifndef CI_LAB_APP_H
#define CI_LAB_APP_H

/*
** Required header files...
*/
#include "common_types.h"
#include "cfe.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>  
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>


/****************************************************************************/

#define CI_LAB_BASE_UDP_PORT 5012
#define CI_LAB_MAX_INGEST    768
#define CI_LAB_PIPE_DEPTH    32
#define MAX_UNIQUE_MSG_IDS 256 

/************************************************************************
** Type Definitions
*************************************************************************/

/* Metadata structure to hold message details */
typedef struct
{
    CFE_SB_MsgId_t MsgId;                      // Message ID from the CCSDS primary header
    CFE_MSG_FcnCode_t CmdCode;                 // Command code from the secondary header (if applicable)
    char TimeOfDay[16];                        // Time of day in HH:MM:SS.mmmmmm format
    CFE_MSG_SequenceCount_t SequenceCount;     // Sequence count from the CCSDS primary header
    CFE_MSG_Size_t MsgLength;                  // Total length of the message
    bool HasSecondaryHeader;                   // Indicates if the message has a secondary header
    CFE_MSG_Type_t MsgType;                    // Message type (command or telemetry)
    CFE_MSG_ApId_t ApId;                       // Application Process ID (APID) from the CCSDS primary header
    CFE_MSG_HeaderVersion_t HeaderVersion;     // Header version from the CCSDS primary header
    CFE_MSG_SegmentationFlag_t SegmentationFlag; // Segmentation flag from the CCSDS primary header
    CFE_TIME_SysTime_t Timestamp;  
    uint32 CurrentInterPacketIntervalMs;                   // Current interval in milliseconds between consecutive packets
    uint32 SlidingWindowMeanIntervalMs;                    // Mean interval over the sliding window
    uint32 SlidingWindowMaxIntervalMs;                     // Maximum interval over the sliding window
    uint32 SlidingWindowMinIntervalMs;                     // Minimum interval over the sliding window
    uint32 FlowLengthInWindow;                // Flow duration (in milliseconds) within the sliding window
    uint32 MessageCountInWindow;              // Number of messages in the sliding window
    uint32 UniqueMessageIDsInWindow;          // Count of distinct message IDs in the sliding window
    uint32 AverageMessageLengthInWindow;      // Average message length within the sliding window
    double MessageRateInWindow;               // Rate of messages per second within the sliding window
    double StdDevMessageLengthInWindow;       // Standard deviation of message lengths in the sliding window
} CI_Metadata_t;

// Structure for tracking unique message IDs
typedef struct {
    CFE_SB_MsgId_t MsgIds[MAX_UNIQUE_MSG_IDS];
    size_t Count;
} UniqueMsgIdTracker;

typedef struct MetadataNode {
    CI_Metadata_t data;
    struct MetadataNode *next;
} MetadataNode;

typedef struct MetadataList {
    MetadataNode *head;
    MetadataNode *tail;
    size_t size;
} MetadataList;


// Global instance declaration (extern) to ensure it's linked properly
extern UniqueMsgIdTracker msgIdTracker;



/****************************************************************************/
/*
** Local function prototypes...
**
** Note: Except for the entry point (CI_LAB_AppMain), these
**       functions are not called from any other source module.
*/
void CI_Lab_AppMain(void);
void CI_LAB_TaskInit(void);
void CI_LAB_ProcessCommandPacket(CFE_SB_Buffer_t *SBBufPtr);
void CI_LAB_ProcessGroundCommand(CFE_SB_Buffer_t *SBBufPtr);
void CI_LAB_ResetCounters_Internal(void);
void CI_LAB_ReadUpLink(void);

bool CI_LAB_VerifyCmdLength(CFE_MSG_Message_t *MsgPtr, size_t ExpectedLength);

// Metadata-related functions
void CI_CollectCCSDSMetadata(CFE_MSG_Message_t *MsgPtr, CI_Metadata_t *Metadata);
void CI_CollectTimeOfDay(char *TimeOfDay, size_t BufferSize);
void CI_CollectData(CFE_MSG_Message_t* MsgPtr, CI_Metadata_t* Metadata);
void CI_UpdateSlidingWindowStatistics(CFE_TIME_SysTime_t currentTime, CI_Metadata_t *metadata);

// Logging functions
void CI_LogMetadataToConsole(const CI_Metadata_t *metadata);
void CI_LogMetadataToCSV(const CI_Metadata_t *metadata, const char *filename, bool writeHeader);


// Function declarations for sliding window
void InitSlidingWindow(MetadataList *list);
void AddToSlidingWindow(MetadataList *list, CI_Metadata_t metadata);
void RemoveFromSlidingWindow(MetadataList *list);
void ClearSlidingWindow(MetadataList *list);
size_t GetSlidingWindowSize(MetadataList *list);



// Function declarations for unique ID tracker

void InitUniqueMsgIdTracker(UniqueMsgIdTracker *tracker);
bool AddUniqueMsgId(UniqueMsgIdTracker *tracker, CFE_SB_MsgId_t msgId);



#endif
