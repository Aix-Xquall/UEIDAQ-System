//===============================================================================================
//
// NAME:    SampleVMap225.c
//
// DESCRIPTION:
//
//          This example reads the data from multiple channels on a single AI-225 layer using
//          VMap mode, and stores the retrieved values to a circular buffer
//          The circular buffer can optionally be copied to file when the program ends
//
// NOTES:
//          This example works only in RT non-DQE VMap mode
//
// ------------------------------------------------------------------------------------------------
//
//      Copyright (C) 2012 United Electronic Industries, Inc.
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


// ----------------- main routine -----------------------------
//
int main(int argc, char* argv[]) {
    PDNA_PARAMS params = { 0, 25, { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24}, 1000.0, 0, 1000, 0, "" };
    int i, totalscans = 0, count = 0;
    int ch, ret;
    int hd, vmapid;
    int act_size;
    int avl_size, data_size;
    DQRDCFG *DQRdCfg = NULL;
    uint32 *bdata = NULL;
    double *fdata = NULL;
    int vmapFlag = DQ_VMAP_FIFO_STATUS;
    double* circularBuffer;
    struct sched_param schedp;
    struct timeval tv1, tv2;
    struct timespec next;
    long long periodns;
    double duration, vmapRefreshRate;
    FILE* fp = NULL;

    ParseParameters(argc, argv, &params);

    signal(SIGINT, sighandler);

    // allocate buffers
    bdata = (uint32*)malloc(params.numChannels*params.numSamplesPerChannel*sizeof(uint32));
    fdata = (double*)malloc(params.numChannels*params.numSamplesPerChannel*sizeof(double));
    circularBuffer = (double*)malloc(params.numChannels * params.numSamplesPerChannel * NUM_FRAMES * sizeof(double));

    // Clear circular buffer
    memset(circularBuffer, 0, params.numChannels * params.numSamplesPerChannel * NUM_FRAMES * sizeof(double));

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
    ret = DqRtVmapInit(hd, &vmapid, params.frequency);
    if(ret < 0) {
        printf("Error %d in DqRtVmapInit\n", ret);
        goto finish_up;
    }

    // configure channels list
    for (ch = 0; ch < params.numChannels; ch++) {
        params.channels[ch] |= DQ_LNCL_GAIN(DQ_AI225_GAIN_1) | DQ_LNCL_DIFF;
    }

    // For AI devices all AI channels are interleaved into one VMAP channel
    ret = DqRtVmapAddChannel(hd, vmapid, params.device, DQ_SS0IN, (int*)params.channels, &vmapFlag, 1);
    if(ret < 0) {
        printf("Error %d in DqRtVmapAddChannel\n", ret);
        goto finish_up;
    }

    ret = DqRtVmapSetScanRate(hd, vmapid, params.device, DQ_SS0IN, params.frequency);
    if(ret < 0) {
        printf("Error %d in DqRtVmapSetScanRate\n", ret);
        goto finish_up;
    }

    // Program channel list on device
    ret = DqRtVmapSetChannelList(hd, vmapid, params.device, DQ_SS0IN, (int*)params.channels, params.numChannels);
    if(ret < 0) {
        printf("Error %d in DqRtVmapSetChannelList\n", ret);
        goto finish_up;
    }

    // Configure hardware timer to tick at a frequency fast enough
    // to at least empty half the fifo at each iteration
    // AI-225 FIFO contains up to 512 samples
    vmapRefreshRate = (params.frequency*params.numChannels)/256;
    periodns = (long long)floor(1000000000.0/vmapRefreshRate);

    ret = DqRtVmapStart(hd, vmapid);
    if(ret < 0) {
        printf("Error %d in DqRtVmapStart\n", ret);
        goto finish_up;
    }

    ret = DqCmdSwTrigger(hd, 1 << params.device);
    if(ret < 0)
    {
        fprintf(stderr, "Error %d in DqCmdSwTrigger\n", ret);
        goto finish_up;
    }

    totalscans = 0;

    gettimeofday(&tv1, NULL);
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!stop) {
        int numScansReceived, firstScanPos;

        // request input data
        ret = DqRtVmapRqInputDataSz(hd, vmapid, 0, params.numSamplesPerChannel*params.numChannels*sizeof(uint32), &act_size, NULL);
        if(ret < 0) {
            printf("Error %d in DqRtVmapRqInputDataSz\n", ret);
            goto finish_up;
        }

        LTRACE("-Requested input requested: %d samples remains: %d\n", act_size/sizeof(uint32), ret/sizeof(uint32));

        // Exchange packets with the cube ------------------
        ret = DqRtVmapRefresh(hd, vmapid, 0);
        if(ret < 0) {
            printf("Error %d in DqRtVmapRefresh, acquired %d scans\n", ret, totalscans);
            goto finish_up;
        }

        // Get received data
        ret = DqRtVmapGetInputData(hd, vmapid, 0, params.numSamplesPerChannel*params.numChannels*sizeof(uint32), &data_size, &avl_size, (uint8*)bdata);
        if(ret < 0) {
            printf("Error %d in DqRtVmapGetInputData\n", ret);
            goto finish_up;
        }

        LTRACE("+Got Input got: %d samples, remains: %d\n", data_size/sizeof(uint32), avl_size/sizeof(uint32));

        numScansReceived = (data_size/sizeof(uint32))/params.numChannels;

        for(i=0; i<numScansReceived; i++) {
            for (ch = 0; ch < params.numChannels; ch++) {
                DqAdvRawToScaleValue(hd, params.device,
                                    params.channels[i%params.numChannels],
                                    ntohl(bdata[i*params.numChannels+ch]),
                                    &fdata[i*params.numChannels+ch]);
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
            printf("Acquired %d scans in %fs (%f scans/s)\n", totalscans, duration, totalscans/duration);
        }

        timespec_add_ns(&next, periodns);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    gettimeofday(&tv2, NULL);
    duration = ((tv2.tv_sec-tv1.tv_sec) + (tv2.tv_usec-tv1.tv_usec)/1000000.0);
    printf("Acquired %d scans in %fs (%f scans/s)\n", totalscans, duration, totalscans/duration);


finish_up:
    ret = DqRtVmapStop(hd, vmapid);           // Stop VMap
    ret = DqRtVmapClose(hd, vmapid);          // Destroy it

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
                    fprintf(fp, "\t%f", circularBuffer[i*params.numChannels+ch]);
                }
                fprintf(fp, "\n");
            }
            for(i=0; i<firstScanPos; i++)
            {
                int ch;
                fprintf(fp, "%d", (params.numSamplesPerChannel*NUM_FRAMES)-firstScanPos+i);
                for(ch=0; ch<params.numChannels; ch++)
                {
                    fprintf(fp, "\t%f", circularBuffer[i*params.numChannels+ch]);
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

