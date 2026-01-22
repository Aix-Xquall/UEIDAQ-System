//===============================================================================================
//
// NAME:    SampleVMap217.c
//
// DESCRIPTION:
//
//          This example reads the data from multiple channels on a single AI-217 layer using
//          VMap mode, and stores the retrieved values to a circular buffer
//          The circular buffer can optionally be copied to file when the program ends
//
// NOTES:
//          This example works only in RT non-DQE VMap mode
//
// ------------------------------------------------------------------------------------------------
//
//      Copyright (C) 2016 United Electronic Industries, Inc.
//      All rights reserved.
//      United Electronic Industries Confidential Information.
//
//=================================================================================================

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sched.h>
#include <unistd.h>

#include "PDNA.h"
#include "UeiPacUtils.h"

//#define DEBUG

#ifdef DEBUG
#define LTRACE printf
#else
#define LTRACE(p...)
#endif

#define NUM_FRAMES     10       // Number of frames in the circular buffer used to store samples
                                // and dump them to file once the program end

static int stop;


// Add specified amount of ns to timespec
static inline void timespec_add_ns(struct timespec *a, unsigned int ns)
{
#define NSECS_PER_SEC 1000000000L
    ns += a->tv_nsec;
    while(ns >= NSECS_PER_SEC) {
        ns -= NSECS_PER_SEC;
        a->tv_sec++;
    }
    a->tv_nsec = ns;
}


// --
// Handler for SIGINT
//
void sighandler(int sig)
{
    stop = 1;
}


void PrintTimeStamp(FILE* fp, double timeStamp)
{
    char datestr[80];
    struct tm* tstime;
    double tssecs, tsmicrosecs;
    time_t tscalendartime;

    // split absolute ts into seconds and microseconds
    tsmicrosecs = modf(timeStamp, &tssecs)*1000000.0;
    // convert number of seconds from double to calendar time
    tscalendartime = (time_t)tssecs;
    // convert calendar time to struct tm
    tstime = localtime(&tscalendartime);

    // format date and time to look pretty
    strftime(datestr,sizeof(datestr)-1,"%Y-%m-%d_%Hh%Mm%Ss",tstime);

    // insert date,time and microseconds in log file
    fprintf(fp, "%s.%06dus", datestr, (int)(tsmicrosecs));
}


// ----------------- main routine -----------------------------
//
int main(int argc, char* argv[]) {
    PDNA_PARAMS params = { 0, 8, { 0,1,2,3,4,5,6,7}, 1000.0, 0, 1000, 0, "" };
    int i, totalscans = 0, count = 0;
    int ch, ret;
    int hd, vmapid;
    int act_size;
    int avl_size, data_size;
    DQRDCFG *DQRdCfg = NULL;
    uint32 *bdata = NULL;
    double *fdata = NULL;
    int vmapFlag = DQ_VMAP_FIFO_STATUS | DQ_VMAP_FIFO_CLR_ON_OVF;
    double* circularBuffer;
    struct sched_param schedp;
    struct timeval tv1, tv2;
    struct timespec next;
    long long periodns;
    double duration, vmapRefreshRate;
    struct timeval startTime;
    double startTimeSeconds;
    FILE* fp = NULL;
    double prevTS, deltaTS;
    int ovfCount = 0;

    ParseParameters(argc, argv, &params);

    signal(SIGINT, sighandler);


    // Configure this process to run with the real-time scheduler
    memset(&schedp, 0, sizeof(schedp));
    schedp.sched_priority = 80;
    sched_setscheduler(0, SCHED_FIFO, &schedp);

    DqInitDAQLib();

    // open communication with IOM and receive IOM crucial identification data
    if ((ret = DqOpenIOM("127.0.0.1", DQ_UDP_DAQ_PORT, 500, &hd, &DQRdCfg)) < 0) {
        printf("Error %d Initializing Communication with IOM\n", ret);
        return -1;
    }


    // Create VMap
    Chk4Err(DqRtVmapInit(hd, &vmapid, params.frequency), goto finish_up);

    // configure channels list
    for (ch = 0; ch < params.numChannels; ch++) {
        params.channels[ch] |= DQ_LNCL_GAIN(DQ_AI217_GAIN_1) | DQ_LNCL_DIFF;
    }

    // Add timestamp channel
    //params.channels[ch] = DQ_LNCL_TIMESTAMP;
    //params.numChannels++;

    // allocate buffers
    bdata = (uint32*)malloc(params.numChannels*params.numSamplesPerChannel*sizeof(uint32));
    fdata = (double*)malloc(params.numChannels*params.numSamplesPerChannel*sizeof(double));
    circularBuffer = (double*)malloc(params.numChannels * params.numSamplesPerChannel * NUM_FRAMES * sizeof(double));

    // Clear circular buffer
    memset(circularBuffer, 0, params.numChannels * params.numSamplesPerChannel * NUM_FRAMES * sizeof(double));

    // For AI devices all AI channels are interleaved into one VMAP channel
    Chk4Err(DqRtVmapAddChannel(hd, vmapid, params.device, DQ_SS0IN, (int*)params.channels, &vmapFlag, 1), goto finish_up);

    Chk4Err(DqRtVmapSetScanRate(hd, vmapid, params.device, DQ_SS0IN, params.frequency), goto finish_up);

    // Program channel list on device
    Chk4Err(DqRtVmapSetChannelList(hd, vmapid, params.device, DQ_SS0IN, (int*)params.channels, params.numChannels), goto finish_up);

    // Configure hardware timer to tick at a frequency fast enough
    // to at least empty half the fifo at each iteration
    // AI-217 FIFO contains up to 2048 samples
    vmapRefreshRate = (params.frequency*params.numChannels)/1024;
    periodns = (long long)floor(1000000000.0/vmapRefreshRate);

    Chk4Err(DqRtVmapStart(hd, vmapid), goto finish_up);

    // Use wall clock time as absolute timestamp for the first sample
    //clock_gettime(CLOCK_REALTIME, &startTime);
    gettimeofday(&startTime, NULL);
    // Convert start time to number of seconds since 00:00:00 UTC 01/01/1970
    startTimeSeconds = startTime.tv_sec + startTime.tv_usec/1.0E6;
    Chk4Err(DqCmdSwTrigger(hd, 1 << params.device), goto finish_up);

    totalscans = 0;
    prevTS = startTimeSeconds;

    gettimeofday(&tv1, NULL);
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!stop) {
        int numScansReceived, firstScanPos;

        // request input data
        Chk4Err(DqRtVmapRqInputDataSz(hd, vmapid, 0, params.numSamplesPerChannel*params.numChannels*sizeof(uint32), &act_size, NULL), goto finish_up);

        LTRACE("-Requested input requested: %d samples remains: %d\n", act_size/sizeof(uint32), ret/sizeof(uint32));

        // Exchange packets with the cube ------------------
        Chk4Err(DqRtVmapRefresh(hd, vmapid, 0), goto finish_up);

        // Get received data
        Chk4Err(DqRtVmapGetInputData(hd, vmapid, 0, params.numSamplesPerChannel*params.numChannels*sizeof(uint32), &data_size, &avl_size, (uint8*)bdata), goto finish_up);

        LTRACE("+Got Input got: %d samples, remains: %d\n", data_size/sizeof(uint32), avl_size/sizeof(uint32));

        numScansReceived = (data_size/sizeof(uint32))/params.numChannels;

        for(i=0; i<numScansReceived; i++)
        {
            for (ch = 0; ch < params.numChannels; ch++)
            {
                if(DQ_LNCL_TIMESTAMP == params.channels[ch])
                {
                    fdata[i*params.numChannels+ch] = startTimeSeconds + ntohl(bdata[i*params.numChannels+ch])/100000.0;
                    // check whether there is a gap in timestamps caused by FIFO overflow
                    if(i>0)
                    {
                        deltaTS = fdata[i*params.numChannels+ch] - fdata[(i-1)*params.numChannels+ch];
                    }
                    else
                    {
                        deltaTS = fdata[i*params.numChannels+ch] - prevTS;
                    }

                    if(deltaTS > (1.0/vmapRefreshRate))
                    {
                        //printf("Detected gap of %f secs in data caused by overflow\n", deltaTS);
                        ovfCount++;
                    }

                    prevTS = fdata[i*params.numChannels+ch];
                }
                else
                {
                    DqAdvRawToScaleValue(hd, params.device,
                                        params.channels[i%params.numChannels],
                                        ntohl(bdata[i*params.numChannels+ch]),
                                        &fdata[i*params.numChannels+ch]);
                }
            }
        }

        // copy received data to circular buffer
        firstScanPos = totalscans % (params.numSamplesPerChannel*NUM_FRAMES);
        if((firstScanPos+numScansReceived) > (params.numSamplesPerChannel*NUM_FRAMES))
        {
            int availableTailScans = (params.numSamplesPerChannel*NUM_FRAMES)-firstScanPos;

            memcpy(circularBuffer+(firstScanPos*params.numChannels),
                   fdata, availableTailScans*params.numChannels*sizeof(double));
            memcpy(circularBuffer,
                   fdata+availableTailScans*params.numChannels, (numScansReceived-availableTailScans)*params.numChannels*sizeof(double));
        }
        else
        {
            memcpy(circularBuffer+(firstScanPos*params.numChannels),
                   fdata, numScansReceived*params.numChannels*sizeof(double));
        }

        totalscans = totalscans + numScansReceived;
        count++;

        // print status once a second
        if(0 == (count % (int)vmapRefreshRate))
        {
            gettimeofday(&tv2, NULL);
            duration = ((tv2.tv_sec-tv1.tv_sec) + (tv2.tv_usec-tv1.tv_usec)/1000000.0);
            printf("Acquired %d scans in %fs (%f scans/s) with %d FIFO overflows\n", totalscans, duration, totalscans/duration, ovfCount);
        }

        timespec_add_ns(&next, periodns);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    gettimeofday(&tv2, NULL);
    duration = ((tv2.tv_sec-tv1.tv_sec) + (tv2.tv_usec-tv1.tv_usec)/1000000.0);
    printf("Acquired %d scans in %fs (%f scans/s)\n", totalscans, duration, totalscans/duration);


finish_up:
    Chk4Err(DqRtVmapStop(hd, vmapid),);           // Stop VMap
    Chk4Err(DqRtVmapClose(hd, vmapid),);          // Destroy it

    if (hd) {
       DqCloseIOM(hd);
    }

    DqCleanUpDAQLib();

    if(bdata != NULL) {
        free(bdata);
    }
    if(fdata != NULL) {
        free(fdata);
    }

    // Dump circular buffer to file if file name was specified
    if(strlen(params.streamFileName) > 0)
    {
       fp = fopen(params.streamFileName, "w+");
        if(NULL == fp)
        {
            fprintf(stderr, "Error opening data file: %s\n", strerror(errno));
        }
        else
        {
            int firstScanPos = totalscans%(params.numSamplesPerChannel*NUM_FRAMES);

            for(i=firstScanPos; i<params.numSamplesPerChannel*NUM_FRAMES; i++)
            {
                int ch;
                fprintf(fp, "%d", i-firstScanPos);
                for(ch=0; ch<params.numChannels; ch++)
                {
                    if(DQ_LNCL_TIMESTAMP == params.channels[ch])
                    {
                        fprintf(fp, "\t");
                        PrintTimeStamp(fp, circularBuffer[i*params.numChannels+ch]);
                    }
                    else
                    {
                        fprintf(fp, "\t%f", circularBuffer[i*params.numChannels+ch]);
                    }
                }
                fprintf(fp, "\n");
            }
            for(i=0; i<firstScanPos; i++)
            {
                int ch;
                fprintf(fp, "%d", (params.numSamplesPerChannel*NUM_FRAMES)-firstScanPos+i);
                for(ch=0; ch<params.numChannels; ch++)
                {
                    if(DQ_LNCL_TIMESTAMP == params.channels[ch])
                    {
                        fprintf(fp, "\t");
                        PrintTimeStamp(fp, circularBuffer[i*params.numChannels+ch]);
                    }
                    else
                    {
                        fprintf(fp, "\t%f", circularBuffer[i*params.numChannels+ch]);
                    }
                }
                fprintf(fp, "\n");
            }

            fclose(fp);
        }
    }

    if (circularBuffer != NULL)
    {
        free(circularBuffer);
    }

    return 0;

}

