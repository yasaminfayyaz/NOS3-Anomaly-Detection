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
 * \file
 *   This file contains the source code for the Command Ingest task.
 */

/*
**   Include Files:
*/
#include <stdio.h>
#include <string.h>
#include <stdbool.h>      
#include <stdlib.h> 
#include <math.h>  
#include "cfe.h"
#include "ci_lab_app.h"
#include "ci_lab_perfids.h"
#include "ci_lab_msgids.h"
#include "ci_lab_msg.h"
#include "ci_lab_events.h"
#include "ci_lab_version.h"

/*
** CI global data...
*/

typedef struct
{
    bool            SocketConnected;
    CFE_SB_PipeId_t CommandPipe;
    osal_id_t       SocketID;
    OS_SockAddr_t   SocketAddress;

    CI_LAB_HkTlm_t HkTlm;

    CFE_SB_Buffer_t *NextIngestBufPtr;

} CI_LAB_GlobalData_t;


CI_LAB_GlobalData_t CI_LAB_Global;


MetadataList SlidingWindow;  // Define the sliding window using a linked list
static uint32 SlidingWindowDurationMs = 60000;  // Window duration in milliseconds (rename for clarity)
UniqueMsgIdTracker msgIdTracker;


/*
 * Individual message handler function prototypes
 *
 * Per the recommended code pattern, these should accept a const pointer
 * to a structure type which matches the message, and return an int32
 * where CFE_SUCCESS (0) indicates successful handling of the message.
 */
int32 CI_LAB_Noop(const CI_LAB_NoopCmd_t *data);
int32 CI_LAB_ResetCounters(const CI_LAB_ResetCountersCmd_t *data);

/* Housekeeping message handler */
int32 CI_LAB_ReportHousekeeping(const CFE_MSG_CommandHeader_t *data);

/** * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                            */
/* Application entry point and main process loop                              */
/* Purpose: This is the Main task event loop for the Command Ingest Task      */
/*            The task handles all interfaces to the data system through      */
/*            the software bus. There is one pipeline into this task          */
/*            The task is scheduled by input into this pipeline.              */
/*            It can receive Commands over this pipeline                      */
/*            and acts accordingly to process them.                           */
/*                                                                            */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * *  * * * * **/
void CI_Lab_AppMain(void)
{
    int32            status;
    uint32           RunStatus = CFE_ES_RunStatus_APP_RUN;
    CFE_SB_Buffer_t *SBBufPtr;

    CFE_ES_PerfLogEntry(CI_LAB_MAIN_TASK_PERF_ID);

    CI_LAB_TaskInit();

    /*
    ** CI Runloop
    */
    while (CFE_ES_RunLoop(&RunStatus) == true)
    {
        CFE_ES_PerfLogExit(CI_LAB_MAIN_TASK_PERF_ID);

        /* Pend on receipt of command packet -- timeout set to 500 millisecs */
        status = CFE_SB_ReceiveBuffer(&SBBufPtr, CI_LAB_Global.CommandPipe, 100);

        CFE_ES_PerfLogEntry(CI_LAB_MAIN_TASK_PERF_ID);

        if (status == CFE_SUCCESS)
        {
            CI_LAB_ProcessCommandPacket(SBBufPtr);
        }

        /* Regardless of packet vs timeout, always process uplink queue      */
        if (CI_LAB_Global.SocketConnected)
        {
            CI_LAB_ReadUpLink();
        }
    }

    CFE_ES_ExitApp(RunStatus);
}

/*
** CI delete callback function.
** This function will be called in the event that the CI app is killed.
** It will close the network socket for CI
*/
void CI_LAB_delete_callback(void)
{
    OS_printf("CI delete callback -- Closing CI Network socket.\n");
    OS_close(CI_LAB_Global.SocketID);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  */
/*                                                                            */
/* CI initialization                                                          */
/*                                                                            */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
void CI_LAB_TaskInit(void)
{
    int32  status;
    uint16 DefaultListenPort;

    memset(&CI_LAB_Global, 0, sizeof(CI_LAB_Global));

    CFE_EVS_Register(NULL, 0, CFE_EVS_EventFilter_BINARY);

    CFE_SB_CreatePipe(&CI_LAB_Global.CommandPipe, CI_LAB_PIPE_DEPTH, "CI_LAB_CMD_PIPE");
    CFE_SB_Subscribe(CFE_SB_ValueToMsgId(CI_LAB_CMD_MID), CI_LAB_Global.CommandPipe);
    CFE_SB_Subscribe(CFE_SB_ValueToMsgId(CI_LAB_SEND_HK_MID), CI_LAB_Global.CommandPipe);

    // Clear the sliding window
    ClearSlidingWindow(&SlidingWindow);
    // Reset the unique ID tracker
    InitUniqueMsgIdTracker(&msgIdTracker);



    status = OS_SocketOpen(&CI_LAB_Global.SocketID, OS_SocketDomain_INET, OS_SocketType_DATAGRAM);
    if (status != OS_SUCCESS)
    {
        CFE_EVS_SendEvent(CI_LAB_SOCKETCREATE_ERR_EID, CFE_EVS_EventType_ERROR, "CI: create socket failed = %d",
                          (int)status);
    }
    else
    {
        OS_SocketAddrInit(&CI_LAB_Global.SocketAddress, OS_SocketDomain_INET);
        DefaultListenPort = CI_LAB_BASE_UDP_PORT + CFE_PSP_GetProcessorId() - 1;
        OS_SocketAddrSetPort(&CI_LAB_Global.SocketAddress, DefaultListenPort);

        status = OS_SocketBind(CI_LAB_Global.SocketID, &CI_LAB_Global.SocketAddress);

        if (status != OS_SUCCESS)
        {
            CFE_EVS_SendEvent(CI_LAB_SOCKETBIND_ERR_EID, CFE_EVS_EventType_ERROR, "CI: bind socket failed = %d",
                              (int)status);
        }
        else
        {
            CI_LAB_Global.SocketConnected = true;
            CFE_ES_WriteToSysLog("CI_LAB listening on UDP port: %u\n", (unsigned int)DefaultListenPort);
        }
    }

    CI_LAB_ResetCounters_Internal();

    /*
    ** Install the delete handler
    */
    OS_TaskInstallDeleteHandler(&CI_LAB_delete_callback);

    CFE_MSG_Init(CFE_MSG_PTR(CI_LAB_Global.HkTlm.TelemetryHeader), CFE_SB_ValueToMsgId(CI_LAB_HK_TLM_MID),
                 sizeof(CI_LAB_Global.HkTlm));

    CFE_EVS_SendEvent(CI_LAB_STARTUP_INF_EID, CFE_EVS_EventType_INFORMATION, "CI Lab Initialized.%s",
                      CI_LAB_VERSION_STRING);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
/*                                                                            */
/*  Purpose:                                                                  */
/*     This routine will process any packet that is received on the CI command*/
/*     pipe. The packets received on the CI command pipe are listed here:     */
/*                                                                            */
/*        1. NOOP command (from ground)                                       */
/*        2. Request to reset telemetry counters (from ground)                */
/*        3. Request for housekeeping telemetry packet (from HS task)         */
/*                                                                            */
/* * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * *  * *  * * * * */
void CI_LAB_ProcessCommandPacket(CFE_SB_Buffer_t *SBBufPtr)
{
    CFE_SB_MsgId_t MsgId = CFE_SB_INVALID_MSG_ID;

    CFE_MSG_GetMsgId(&SBBufPtr->Msg, &MsgId);

    switch (CFE_SB_MsgIdToValue(MsgId))
    {
        case CI_LAB_CMD_MID:
            CI_LAB_ProcessGroundCommand(SBBufPtr);
            break;

        case CI_LAB_SEND_HK_MID:
            CI_LAB_ReportHousekeeping((const CFE_MSG_CommandHeader_t *)SBBufPtr);
            break;

        default:
            CI_LAB_Global.HkTlm.Payload.CommandErrorCounter++;
            CFE_EVS_SendEvent(CI_LAB_COMMAND_ERR_EID, CFE_EVS_EventType_ERROR, "CI: invalid command packet,MID = 0x%x",
                              (unsigned int)CFE_SB_MsgIdToValue(MsgId));
            break;
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
/*                                                                            */
/* CI ground commands                                                         */
/*                                                                            */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/

void CI_LAB_ProcessGroundCommand(CFE_SB_Buffer_t *SBBufPtr)
{
    CFE_MSG_FcnCode_t CommandCode = 0;

    CFE_MSG_GetFcnCode(&SBBufPtr->Msg, &CommandCode);

    /* Process "known" CI task ground commands */
    switch (CommandCode)
    {
        case CI_LAB_NOOP_CC:
            if (CI_LAB_VerifyCmdLength(&SBBufPtr->Msg, sizeof(CI_LAB_NoopCmd_t)))
            {
                CI_LAB_Noop((const CI_LAB_NoopCmd_t *)SBBufPtr);
            }
            break;

        case CI_LAB_RESET_COUNTERS_CC:
            if (CI_LAB_VerifyCmdLength(&SBBufPtr->Msg, sizeof(CI_LAB_ResetCountersCmd_t)))
            {
                CI_LAB_ResetCounters((const CI_LAB_ResetCountersCmd_t *)SBBufPtr);
            }
            break;

        /* default case already found during FC vs length test */
        default:
            break;
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
/*                                                                            */
/* Linked List Functionality                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/


void InitSlidingWindow(MetadataList *list) {
    list->head = NULL;
    list->tail = NULL;
    list->size = 0;
}


void AddToSlidingWindow(MetadataList *list, CI_Metadata_t metadata) {
    MetadataNode *newNode = (MetadataNode *)malloc(sizeof(MetadataNode));
    if (newNode == NULL) {
        printf("CI_LAB - Memory allocation failed\n");
        return;
    }
    newNode->data = metadata;
    newNode->next = NULL;

    if (list->tail == NULL) {
        // List is empty, head and tail point to the new node
        list->head = newNode;
        list->tail = newNode;
    } else {
        // Add to the end of the list
        list->tail->next = newNode;
        list->tail = newNode;
    }
    list->size++;
    
    // Debug: Print new node addition
    printf("CI_LAB - Added MsgId: 0x%04X to Sliding Window. New size: %zu\n",
           CFE_SB_MsgIdToValue(metadata.MsgId), list->size);
}


void RemoveFromSlidingWindow(MetadataList *list) {
    if (list->head == NULL) {
        // List is empty
        return;
    }
    MetadataNode *temp = list->head;
    list->head = list->head->next;

    if (list->head == NULL) {
        // List is now empty, update tail
        list->tail = NULL;
    }
    free(temp);
    list->size--;
}

size_t GetSlidingWindowSize(MetadataList *list) {
    return list->size;
}

void ClearSlidingWindow(MetadataList *list) {
    MetadataNode *current = list->head;
    MetadataNode *temp;

    while (current != NULL) {
        temp = current;
        current = current->next;
        free(temp);  // Free each node
    }

    list->head = NULL;
    list->tail = NULL;
    list->size = 0;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
/*  Define a Structure to Track Unique IDs
                                                                              */
/*                                                                            */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/



void InitUniqueMsgIdTracker(UniqueMsgIdTracker *tracker) {
    tracker->Count = 0;
    memset(tracker->MsgIds, 0, sizeof(tracker->MsgIds));
}

bool AddUniqueMsgId(UniqueMsgIdTracker *tracker, CFE_SB_MsgId_t msgId) {
    // Convert the msgId to an integer for comparison
    CFE_SB_MsgId_Atom_t msgIdValue = CFE_SB_MsgIdToValue(msgId);

    // Check if msgId already exists by comparing its integer value
    for (size_t i = 0; i < tracker->Count; ++i) {
        if (CFE_SB_MsgIdToValue(tracker->MsgIds[i]) == msgIdValue) {
            return false; // Already exists
        }
    }
    
    // If the MsgId is not found, add it
    if (tracker->Count < MAX_UNIQUE_MSG_IDS) {
        tracker->MsgIds[tracker->Count++] = msgId;
        return true;
    } else {
        printf("Warning: Unique message ID tracker is full.\n");
        return false;
    }
}
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
/*                                                                            */
/* CI Metadata Collection                                                     */
/*                                                                            */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
/**
 * @brief Collects metadata from a CCSDS message
 *
 * This function extracts metadata from the given CCSDS message and populates
 * the provided CI_Metadata_t structure with the extracted information.
 *
 * @param[in]  MsgPtr    Pointer to the message from which to collect metadata
 * @param[out] Metadata  Pointer to the structure where metadata will be stored
 */
void CI_CollectCCSDSMetadata(CFE_MSG_Message_t* MsgPtr, CI_Metadata_t* Metadata)
{
    if (MsgPtr == NULL || Metadata == NULL)
    {
        printf("CI_LAB - Invalid message or metadata pointer.\n");
        return;
    }

    /* Collect the Message ID from the CCSDS primary header */
    CFE_MSG_GetMsgId(MsgPtr, &Metadata->MsgId);

    /* Collect the Function Code from the secondary header (if applicable) */
    CFE_MSG_GetFcnCode(MsgPtr, &Metadata->CmdCode);

    /* Collect the Sequence Count from the CCSDS primary header */
    CFE_MSG_GetSequenceCount(MsgPtr, &Metadata->SequenceCount);

    /* Collect the Total Message Length */
    CFE_MSG_GetSize(MsgPtr, &Metadata->MsgLength);

    /* Check if the message has a secondary header */
    CFE_MSG_GetHasSecondaryHeader(MsgPtr, &Metadata->HasSecondaryHeader);

    /* Collect the Message Type (command or telemetry) from the CCSDS primary header */
    CFE_MSG_GetType(MsgPtr, &Metadata->MsgType);

    /* Collect the Application Process ID (APID) from the CCSDS primary header */
    CFE_MSG_GetApId(MsgPtr, &Metadata->ApId);

    /* Collect the Header Version from the CCSDS primary header */
    CFE_MSG_GetHeaderVersion(MsgPtr, &Metadata->HeaderVersion);

    /* Collect the Segmentation Flag from the CCSDS primary header */
    CFE_MSG_GetSegmentationFlag(MsgPtr, &Metadata->SegmentationFlag);
    
}
void CI_CollectTimeOfDay(char *TimeOfDay, size_t BufferSize) {
    if (TimeOfDay == NULL || BufferSize < 16) {
        printf("CI_LAB - Invalid time buffer.\n");
        return;
    }

    // Get the current time using cFE Time Services
    CFE_TIME_SysTime_t CurrentTime = CFE_TIME_GetTime();
    uint32 Seconds = CurrentTime.Seconds;          
    uint32 Subseconds = CurrentTime.Subseconds;

    // Convert Subseconds to Microseconds
    uint32 Microseconds = CFE_TIME_Sub2MicroSecs(Subseconds);

    // Extract time-of-day components from total seconds
    uint32 Hours = (Seconds / 3600) % 24;
    uint32 Minutes = (Seconds / 60) % 60;
    uint32 TimeSeconds = Seconds % 60;

    // Format the Time of Day string in HH:MM:SS.mmmmmm format
    snprintf(TimeOfDay, BufferSize, "%02u:%02u:%02u.%06u", Hours, Minutes, TimeSeconds, Microseconds);
}
void CI_UpdateSlidingWindowStatistics(CFE_TIME_SysTime_t currentTime, CI_Metadata_t *metadata) {
    if (metadata == NULL) {
        printf("CI_LAB - Invalid metadata pointer.\n");
        return;
    }

    // Debug: Print metadata timestamp
    printf("CI_LAB - Metadata Timestamp: Seconds=%u, Subseconds=%u\n", metadata->Timestamp.Seconds, metadata->Timestamp.Subseconds);

    // Add to sliding window
    AddToSlidingWindow(&SlidingWindow, *metadata);

    // Remove old messages
    while (SlidingWindow.head != NULL) {
        CFE_TIME_SysTime_t oldestTime = SlidingWindow.head->data.Timestamp;
        CFE_TIME_SysTime_t deltaTime = CFE_TIME_Subtract(currentTime, oldestTime);

        uint32_t deltaMilliseconds = (deltaTime.Seconds * 1000) + (CFE_TIME_Sub2MicroSecs(deltaTime.Subseconds) / 1000);

        // Debug: Print delta time
        printf("CI_LAB - Delta Time: Seconds=%u, Subseconds=%u, DeltaMs=%u\n", 
                deltaTime.Seconds, deltaTime.Subseconds, deltaMilliseconds);

        if (deltaMilliseconds > SlidingWindowDurationMs) {
            printf("CI_LAB - Removing message older than sliding window duration.\n");
            RemoveFromSlidingWindow(&SlidingWindow);
        } else {
            break;
        }
    }
    // Calculate Message Statistics in the Sliding Window
    metadata->MessageCountInWindow = GetSlidingWindowSize(&SlidingWindow);

    // Track unique message IDs
    UniqueMsgIdTracker msgIdTracker;
    InitUniqueMsgIdTracker(&msgIdTracker);

    MetadataNode *current = SlidingWindow.head;
    uint32 totalMessageLength = 0;
    double sumSquaredDifferences = 0.0;

    while (current != NULL) {
        AddUniqueMsgId(&msgIdTracker, current->data.MsgId);
        totalMessageLength += current->data.MsgLength;
        current = current->next;
    }

    metadata->UniqueMessageIDsInWindow = msgIdTracker.Count;

    if (SlidingWindow.size > 0) {
        metadata->AverageMessageLengthInWindow = totalMessageLength / SlidingWindow.size;

        // Calculate standard deviation of message lengths
        double meanMessageLength = metadata->AverageMessageLengthInWindow;
        current = SlidingWindow.head;
        while (current != NULL) {
            double difference = current->data.MsgLength - meanMessageLength;
            sumSquaredDifferences += (difference * difference);
            current = current->next;
        }
        metadata->StdDevMessageLengthInWindow = sqrt(sumSquaredDifferences / SlidingWindow.size);
    } else {
        metadata->AverageMessageLengthInWindow = 0;
        metadata->StdDevMessageLengthInWindow = 0;
    }

    // Calculate Message Rate
    if (SlidingWindowDurationMs > 0) {  // Avoid division by zero
        metadata->MessageRateInWindow = (double)SlidingWindow.size / (SlidingWindowDurationMs / 1000.0);
    } else {
        metadata->MessageRateInWindow = 0;
    }

    // Calculate Flow Duration in the Sliding Window
    metadata->FlowLengthInWindow = 0;
    if (SlidingWindow.size > 1) {
        CFE_TIME_SysTime_t oldestTime = SlidingWindow.head->data.Timestamp;
        CFE_TIME_SysTime_t deltaTime = CFE_TIME_Subtract(currentTime, oldestTime);
        metadata->FlowLengthInWindow = (deltaTime.Seconds * 1000) + (CFE_TIME_Sub2MicroSecs(deltaTime.Subseconds) / 1000);
    }

    // Calculate Inter-Packet Interval Statistics (Mean, Min, Max)
    if (SlidingWindow.size > 1) {
        uint32 sumInterval = 0;
        uint32 minInterval = UINT32_MAX;
        uint32 maxInterval = 0;

        MetadataNode *current = SlidingWindow.head;
        MetadataNode *prev = NULL;

        while (current != NULL) {
            if (prev != NULL) {
                CFE_TIME_SysTime_t timeDiff = CFE_TIME_Subtract(current->data.Timestamp, prev->data.Timestamp);
                uint32 intervalMs = (timeDiff.Seconds * 1000) + (CFE_TIME_Sub2MicroSecs(timeDiff.Subseconds) / 1000);
                sumInterval += intervalMs;

                if (intervalMs < minInterval) minInterval = intervalMs;
                if (intervalMs > maxInterval) maxInterval = intervalMs;
            }
            prev = current;
            current = current->next;
        }

        metadata->SlidingWindowMeanIntervalMs = sumInterval / (SlidingWindow.size - 1);
        metadata->SlidingWindowMinIntervalMs = minInterval;
        metadata->SlidingWindowMaxIntervalMs = maxInterval;
    } else {
        metadata->SlidingWindowMeanIntervalMs = 0;
        metadata->SlidingWindowMinIntervalMs = 0;
        metadata->SlidingWindowMaxIntervalMs = 0;
    }
}



void CI_CollectData(CFE_MSG_Message_t* MsgPtr, CI_Metadata_t* Metadata) {
    if (MsgPtr == NULL || Metadata == NULL) {
        printf("CI_LAB - Invalid message or metadata pointer.\n");
        return;
    }

    // Collect metadata using dedicated functions
    CI_CollectCCSDSMetadata(MsgPtr, Metadata);
    CI_CollectTimeOfDay(Metadata->TimeOfDay, sizeof(Metadata->TimeOfDay));

    // Retrieve current time from cFE Time Services
    CFE_TIME_SysTime_t currentTime = CFE_TIME_GetTime();


    // Debug: Print current time
    printf("CI_LAB - CurrentTime before assignment: Seconds=%u, Subseconds=%u\n", currentTime.Seconds, currentTime.Subseconds);

    // Assign Timestamp
    Metadata->Timestamp = currentTime;

    // Debug: Print assigned Timestamp
    printf("CI_LAB - Assigned Timestamp: Seconds=%u, Subseconds=%u\n", Metadata->Timestamp.Seconds, Metadata->Timestamp.Subseconds);

    // Update Sliding Window Statistics
    CI_UpdateSlidingWindowStatistics(currentTime, Metadata);

    


}
/**
 * @brief Logs the collected metadata to the console in human-readable format
 *
 * This function prints the metadata collected from the message to the console,
 * including interval timing information.
 *
 * @param[in] metadata Pointer to the metadata structure to log
 */
void CI_LogMetadataToConsole(const CI_Metadata_t *metadata) {
    if (metadata == NULL) {
        printf("CI_LAB - Invalid metadata pointer.\n");
        return;
    }

    // Convert Message Type to string for display
    const char* msgTypeStr = (metadata->MsgType == CFE_MSG_Type_Cmd) ? "Command" : "Telemetry";

    // Convert Segmentation Flag to string for display
    const char* segFlagStr = "";
    switch (metadata->SegmentationFlag) {
        case CFE_MSG_SegFlag_Continue:
            segFlagStr = "Continue";
            break;
        case CFE_MSG_SegFlag_First:
            segFlagStr = "First";
            break;
        case CFE_MSG_SegFlag_Last:
            segFlagStr = "Last";
            break;
        case CFE_MSG_SegFlag_Unsegmented:
            segFlagStr = "Unsegmented";
            break;
        default:
            segFlagStr = "Invalid";
            break;
    }

    // Print the collected metadata to the console
    printf("Metadata - MsgId: 0x%04X, CmdCode: %d, Time: %s, SeqCount: %d, "
           "MsgLength: %zu, HasSecondaryHeader: %d, MsgType: %s, ApId: %d, "
           "HeaderVersion: %d, SegmentationFlag: %s, FlowLengthInWindow: %u ms, "
           "SlidingWindowMeanIntervalMs: %u, SlidingWindowMaxIntervalMs: %u, SlidingWindowMinIntervalMs: %u, "
           "MessageCountInWindow: %u, UniqueMessageIDsInWindow: %u, AverageMessageLengthInWindow: %u, "
           "MessageRateInWindow: %.2f, StdDevMessageLengthInWindow: %.2f\n",
           CFE_SB_MsgIdToValue(metadata->MsgId), metadata->CmdCode, metadata->TimeOfDay,
           metadata->SequenceCount, metadata->MsgLength, metadata->HasSecondaryHeader,
           msgTypeStr, metadata->ApId, metadata->HeaderVersion, segFlagStr,
           metadata->FlowLengthInWindow, metadata->SlidingWindowMeanIntervalMs,
           metadata->SlidingWindowMaxIntervalMs, metadata->SlidingWindowMinIntervalMs,
           metadata->MessageCountInWindow, metadata->UniqueMessageIDsInWindow,
           metadata->AverageMessageLengthInWindow, metadata->MessageRateInWindow,
           metadata->StdDevMessageLengthInWindow);

}



/**
 * @brief Logs the collected metadata to a CSV file in numerical format
 *
 * This function writes the metadata collected from the message to a CSV file,
 * using raw numerical values suitable for machine learning models.
 *
 * @param[in] metadata    Pointer to the metadata structure to log
 * @param[in] filename    Name of the CSV file to write to
 * @param[in] writeHeader Boolean indicating whether to write the CSV header
 */
void CI_LogMetadataToCSV(const CI_Metadata_t *metadata, const char *filename, bool writeHeader) {
    // Open the file in append mode
    FILE *file = fopen(filename, "a");
    if (file == NULL) {
        printf("CI_LAB - Failed to open file %s for writing.\n", filename);
        perror("Error");  // Print system error message
        return;
    }

    // Write the header if needed
    if (writeHeader) {
        fprintf(file,
                "MsgId,CmdCode,TimeOfDay,SequenceCount,MsgLength,HasSecondaryHeader,MsgType,ApId,HeaderVersion,SegmentationFlag,"
                "FlowLengthInWindow,SlidingWindowMeanIntervalMs,SlidingWindowMaxIntervalMs,SlidingWindowMinIntervalMs,"
                "MessageCountInWindow,UniqueMessageIDsInWindow,AverageMessageLengthInWindow,MessageRateInWindow,StdDevMessageLengthInWindow\n");
    }

    // Write the metadata to the file in CSV format
    fprintf(file,
            "0x%04X,%d,%s,%d,%zu,%d,%d,%d,%d,%d,"
            "%u,%u,%u,%u,"
            "%u,%u,%u,%.2f,%.2f\n",
            CFE_SB_MsgIdToValue(metadata->MsgId), // MsgId in hex format
            metadata->CmdCode,
            metadata->TimeOfDay,
            metadata->SequenceCount,
            metadata->MsgLength,
            metadata->HasSecondaryHeader,
            metadata->MsgType,
            metadata->ApId,
            metadata->HeaderVersion,
            metadata->SegmentationFlag,
            metadata->FlowLengthInWindow,
            metadata->SlidingWindowMeanIntervalMs,
            metadata->SlidingWindowMaxIntervalMs,
            metadata->SlidingWindowMinIntervalMs,
            metadata->MessageCountInWindow,
            metadata->UniqueMessageIDsInWindow,
            metadata->AverageMessageLengthInWindow,
            metadata->MessageRateInWindow,
            metadata->StdDevMessageLengthInWindow);

    // Close the file
    fclose(file);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                             */
/*  Purpose:                                                                   */
/*     Handle NOOP command packets                                             */
/*                                                                             */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
int32 CI_LAB_Noop(const CI_LAB_NoopCmd_t *data)
{
    /* Does everything the name implies */
    CI_LAB_Global.HkTlm.Payload.CommandCounter++;

    CFE_EVS_SendEvent(CI_LAB_COMMANDNOP_INF_EID, CFE_EVS_EventType_INFORMATION, "CI: NOOP command");

    return CFE_SUCCESS;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                             */
/*  Purpose:                                                                   */
/*     Handle ResetCounters command packets                                    */
/*                                                                             */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
int32 CI_LAB_ResetCounters(const CI_LAB_ResetCountersCmd_t *data)
{
    CFE_EVS_SendEvent(CI_LAB_COMMANDRST_INF_EID, CFE_EVS_EventType_INFORMATION, "CI: RESET command");
    ClearSlidingWindow(&SlidingWindow);   
    InitUniqueMsgIdTracker(&msgIdTracker);

    CI_LAB_ResetCounters_Internal();
    return CFE_SUCCESS;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
/*                                                                            */
/*  Purpose:                                                                  */
/*         This function is triggered in response to a task telemetry request */
/*         from the housekeeping task. This function will gather the CI task  */
/*         telemetry, packetize it and send it to the housekeeping task via   */
/*         the software bus                                                   */
/*                                                                            */
/* * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * *  * *  * * * * */
int32 CI_LAB_ReportHousekeeping(const CFE_MSG_CommandHeader_t *data)
{
    CI_LAB_Global.HkTlm.Payload.SocketConnected = CI_LAB_Global.SocketConnected;
    CFE_SB_TimeStampMsg(CFE_MSG_PTR(CI_LAB_Global.HkTlm.TelemetryHeader));
    CFE_SB_TransmitMsg(CFE_MSG_PTR(CI_LAB_Global.HkTlm.TelemetryHeader), true);
    return CFE_SUCCESS;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
/*                                                                            */
/*  Purpose:                                                                  */
/*         This function resets all the global counter variables that are     */
/*         part of the task telemetry.                                        */
/*                                                                            */
/* * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * *  * *  * * * * */
void CI_LAB_ResetCounters_Internal(void)
{
    /* Status of commands processed by CI task */
    CI_LAB_Global.HkTlm.Payload.CommandCounter      = 0;
    CI_LAB_Global.HkTlm.Payload.CommandErrorCounter = 0;

    /* Status of packets ingested by CI task */
    CI_LAB_Global.HkTlm.Payload.IngestPackets = 0;
    CI_LAB_Global.HkTlm.Payload.IngestErrors  = 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
/*                                                                            */
/* --                                                                         */
/*                                                                            */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
void CI_LAB_ReadUpLink(void)
{
    int    i;
    int32  status;
    uint8 *bytes;
    CI_Metadata_t Metadata;
    static bool firstWrite = true;
    const char *filename = "/home/jstar/Desktop/github-nos3/fsw/build/exe/cpu1/cf/metadata_log.csv";



    for (i = 0; i <= 10; i++)
    {
        if (CI_LAB_Global.NextIngestBufPtr == NULL)
        {
            CI_LAB_Global.NextIngestBufPtr = CFE_SB_AllocateMessageBuffer(CI_LAB_MAX_INGEST);
            if (CI_LAB_Global.NextIngestBufPtr == NULL)
            {
                CFE_EVS_SendEvent(CI_LAB_INGEST_ALLOC_ERR_EID, CFE_EVS_EventType_ERROR,
                                  "CI: L%d, buffer allocation failed\n", __LINE__);
                break;
            }
        }

        status = OS_SocketRecvFrom(CI_LAB_Global.SocketID, CI_LAB_Global.NextIngestBufPtr, CI_LAB_MAX_INGEST,
                                   &CI_LAB_Global.SocketAddress, OS_CHECK);
        if (status >= (int32)sizeof(CFE_MSG_CommandHeader_t) && status <= ((int32)CI_LAB_MAX_INGEST))
        {
            CFE_ES_PerfLogEntry(CI_LAB_SOCKET_RCV_PERF_ID);
            CI_LAB_Global.HkTlm.Payload.IngestPackets++;

            // Collect metadata using the new function
            CI_CollectData(&CI_LAB_Global.NextIngestBufPtr->Msg, &Metadata);

            // Debug: Print Timestamp
            printf("CI_LAB - Received Message Timestamp: Seconds=%u, Subseconds=%u\n", 
                   Metadata.Timestamp.Seconds, Metadata.Timestamp.Subseconds);



            // Log to console
            CI_LogMetadataToConsole(&Metadata);

            // Log to CSV file
            CI_LogMetadataToCSV(&Metadata, filename, firstWrite);
            firstWrite = false;


            status = CFE_SB_TransmitBuffer(CI_LAB_Global.NextIngestBufPtr, false);
            CFE_ES_PerfLogExit(CI_LAB_SOCKET_RCV_PERF_ID);

            if (status == CFE_SUCCESS)
            {
                /* Set NULL so a new buffer will be obtained next time around */
                CI_LAB_Global.NextIngestBufPtr = NULL;
            }
            else
            {
                CFE_EVS_SendEvent(CI_LAB_INGEST_SEND_ERR_EID, CFE_EVS_EventType_ERROR,
                                  "CI: L%d, CFE_SB_TransmitBuffer() failed, status=%d\n", __LINE__, (int)status);
            }
        }
        else if (status > 0)
        {
            /* bad size, report as ingest error */
            CI_LAB_Global.HkTlm.Payload.IngestErrors++;

            bytes = CI_LAB_Global.NextIngestBufPtr->Msg.Byte;
            CFE_EVS_SendEvent(CI_LAB_INGEST_LEN_ERR_EID, CFE_EVS_EventType_ERROR,
                              "CI: L%d, cmd %0x%0x %0x%0x dropped, bad length=%d\n", __LINE__, bytes[0], bytes[1],
                              bytes[2], bytes[3], (int)status);
        }
        else
        {
            break; /* no (more) messages */
        }
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
/*                                                                            */
/* Verify command packet length                                               */
/*                                                                            */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
bool CI_LAB_VerifyCmdLength(CFE_MSG_Message_t *MsgPtr, size_t ExpectedLength)
{
    bool              result       = true;
    size_t            ActualLength = 0;
    CFE_MSG_FcnCode_t FcnCode      = 0;
    CFE_SB_MsgId_t    MsgId        = CFE_SB_INVALID_MSG_ID;

    CFE_MSG_GetSize(MsgPtr, &ActualLength);

    /*
    ** Verify the command packet length...
    */
    if (ExpectedLength != ActualLength)
    {
        CFE_MSG_GetMsgId(MsgPtr, &MsgId);
        CFE_MSG_GetFcnCode(MsgPtr, &FcnCode);

        CFE_EVS_SendEvent(CI_LAB_LEN_ERR_EID, CFE_EVS_EventType_ERROR,
                          "Invalid msg length: ID = 0x%X,  CC = %u, Len = %u, Expected = %u",
                          (unsigned int)CFE_SB_MsgIdToValue(MsgId), (unsigned int)FcnCode, (unsigned int)ActualLength,
                          (unsigned int)ExpectedLength);
        result = false;
        CI_LAB_Global.HkTlm.Payload.CommandErrorCounter++;
    }

    return result;
}
