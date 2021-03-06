// Copyright (C) <2019> Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0

#include <zconf.h>
#include <dlfcn.h>
#include "VideoGstAnalyzer.h"
#include <iostream>
#include <string>
#include <unistd.h>

namespace mcu {

DEFINE_LOGGER(VideoGstAnalyzer, "mcu.VideoGstAnalyzer");

GMainLoop* VideoGstAnalyzer::loop = NULL;

VideoGstAnalyzer::VideoGstAnalyzer() {
    ELOG_INFO("Init");
    sourceid = 0;
    sink = NULL;
    fp = NULL;
    encoder_pad = NULL;
}

VideoGstAnalyzer::~VideoGstAnalyzer() {
    ELOG_DEBUG("Closed all media in this Analyzer");
    if (pipeline_ != nullptr && pipelineHandle != nullptr) {
         destroyPlugin(pipeline_);
         dlclose(pipelineHandle);
    }
}

gboolean VideoGstAnalyzer::StreamEventCallBack(GstBus *bus, GstMessage *message, gpointer data)
    {
        ELOG_DEBUG("Got %s message\n", GST_MESSAGE_TYPE_NAME(message));
 
        VideoGstAnalyzer* pStreamObj = static_cast<VideoGstAnalyzer*>(data);
 
        switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *err;
            gchar *debug;
            gst_message_parse_error(message, &err, &debug);
            ELOG_ERROR("Error: %s\n", err->message);
            g_error_free(err);
            g_free(debug);
            g_main_loop_quit(pStreamObj->loop);
    
            break;
        }
        case GST_MESSAGE_EOS:
            /* end-of-stream */
            ELOG_ERROR("End of stream\n");
            g_main_loop_quit(pStreamObj->loop);
            break;
        case GST_MESSAGE_TAG:{
            /* end-of-stream */
            GstTagList *tags = NULL;
            gst_message_parse_tag (message, &tags);

            ELOG_DEBUG("Got tags from element %s:\n", GST_OBJECT_NAME (message->src));
            gst_tag_list_unref (tags);
            break;
        }
        case GST_MESSAGE_QOS:{
            /* end-of-stream */
            ELOG_DEBUG("Got QOS message from %s \n",message->src->name);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED:{
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(message, &old_state, &new_state, &pending_state);
            ELOG_DEBUG("State change from %d to %d, play:%d \n",old_state, new_state, GST_STATE_PAUSED);
            break;
        }
        default:
            /* unhandled message */
            break;
        }
        return true;
    }

void VideoGstAnalyzer::clearPipeline()
    {
        if (pipeline != nullptr){
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(GST_OBJECT(pipeline));
            g_source_remove(m_bus_watch_id);
            g_main_loop_unref(loop);
        }
 
    }

int VideoGstAnalyzer::createPipeline() {

    pipelineHandle = dlopen(libraryName.c_str(), RTLD_LAZY);
    if (pipelineHandle == nullptr) {
        ELOG_ERROR_T("Failed to open the plugin.(%s)", libraryName.c_str());
        return -1;
    }

    createPlugin = (rva_create_t*)dlsym(pipelineHandle, "CreatePipeline");
    destroyPlugin = (rva_destroy_t*)dlsym(pipelineHandle, "DestroyPipeline");

    if (createPlugin == nullptr || destroyPlugin == nullptr) {
        ELOG_ERROR_T("Failed to get plugin interface.");
        dlclose(pipelineHandle);
        return -1;
    }

    pipeline_ = createPlugin();
    if (pipeline_ == nullptr) {
        ELOG_ERROR_T("Failed to create the plugin.");
        dlclose(pipelineHandle);
        return -1;
    }

    std::unordered_map<std::string, std::string> plugin_config_map = {
        {"inputwidth", std::to_string(width)},
        {"inputheight", std::to_string(height)},
        {"inputframerate", std::to_string(framerate)},
        {"pipelinename", algo} };
    pipeline_->PipelineConfig(plugin_config_map);

    /* Create the empty VideoGstAnalyzer */
    pipeline = pipeline_->InitializePipeline();

    if (!pipeline) {
        ELOG_ERROR("pipeline Initialization failed\n");
        return -1;
    }

    loop = g_main_loop_new(NULL, FALSE);

    m_bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    m_bus_watch_id = gst_bus_add_watch(m_bus, StreamEventCallBack, this);
 
    gst_object_unref(m_bus);

    return 0;
};

void VideoGstAnalyzer::start_feed (GstElement * source, guint size, gpointer data)
{
    VideoGstAnalyzer* pStreamObj = static_cast<VideoGstAnalyzer*>(data);
    pStreamObj->m_internalin->setPushData(true);
}

void VideoGstAnalyzer::stop_feed (GstElement * source, gpointer data)
{
    VideoGstAnalyzer* pStreamObj = static_cast<VideoGstAnalyzer*>(data);
    pStreamObj->m_internalin->setPushData(false);
    
}

void VideoGstAnalyzer::new_sample_from_sink (GstElement * source, gpointer data)
{
    ELOG_DEBUG("Got new sample from sink\n");
    VideoGstAnalyzer* pStreamObj = static_cast<VideoGstAnalyzer*>(data);
    GstSample *sample;
    GstBuffer *buffer;

    /* get the sample from appsink */
    sample = gst_app_sink_pull_sample (GST_APP_SINK (pStreamObj->sink));
    buffer = gst_sample_get_buffer (sample);

    GstMapInfo map;
    gst_buffer_map (buffer, &map, GST_MAP_READ);
    
    for ( auto& x: pStreamObj->m_internalout)
        x->onFrame(map.data, pStreamObj->width, pStreamObj->height, map.size);

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
}

int VideoGstAnalyzer::addElementMany() {
    if(pipeline_){
        rvaStatus status = pipeline_->LinkElements();
        if(status != RVA_ERR_OK) {
           ELOG_ERROR("Link element failed with rvastatus:%d\n",status);
           return -1; 
        }
    }

    source = gst_bin_get_by_name (GST_BIN (pipeline), "appsource");
    if (!source) {
        ELOG_ERROR("appsrc in pipeline does not be created\n");
        return -1;
    }

    sink = gst_bin_get_by_name (GST_BIN (pipeline), "appsink");
    if (!sink) {
        ELOG_ERROR("There is no appsink in pipeline\n");
    }

    g_signal_connect (source, "need-data", G_CALLBACK (start_feed), this);
    g_signal_connect (source, "enough-data", G_CALLBACK (stop_feed), this);

    return 0;
}


void VideoGstAnalyzer::stopLoop(){
    if(loop){
        ELOG_DEBUG("main loop quit\n");
        g_main_loop_quit(loop);
    }
    g_thread_join(m_thread);
}

void VideoGstAnalyzer::main_loop_thread(gpointer data){
    g_main_loop_run(loop);
    g_thread_exit(0);
}

void VideoGstAnalyzer::setState() {
    ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        ELOG_ERROR("Unable to set the pipeline to the PLAYING state.\n");
        gst_object_unref(pipeline);
    }
} 


int VideoGstAnalyzer::setPlaying() {

    m_playthread = boost::thread(boost::bind(&VideoGstAnalyzer::setState, this));

    m_thread = g_thread_create((GThreadFunc)main_loop_thread,NULL,TRUE,NULL);

    return 0;
}

void VideoGstAnalyzer::emitListenTo(int minPort, int maxPort) {
    ELOG_DEBUG("Listening\n");
    m_internalin.reset(new InternalIn((GstAppSrc*)source, minPort, maxPort));  
}

void VideoGstAnalyzer::emitConnectTo(int connectionID, char* ip, int remotePort){
    if (sink != nullptr){
        ELOG_DEBUG("Connect to remote ip %s port %d with connetionID %d\n", ip, remotePort);
        
    }
    else {
        ELOG_ERROR("No appsink in the pipeline\n");
    }
}

void VideoGstAnalyzer::addOutput(int connectionID, owt_base::InternalOut* out) {
    ELOG_DEBUG("Add analyzed stream back to OWT\n");
    if (sink != nullptr){
        if(encoder_pad == nullptr) {
            GstElement *encoder = gst_bin_get_by_name (GST_BIN (pipeline), "encoder");
            encoder_pad = gst_element_get_static_pad(encoder, "src");
            out->setPad(encoder_pad);
            ELOG_ERROR("Set encoder pad to internal output\n");
        }
        m_internalout.push_back(out);
        g_object_set (G_OBJECT (sink), "emit-signals", TRUE, "sync", FALSE, NULL);
        g_signal_connect (sink, "new-sample", G_CALLBACK (new_sample_from_sink), this);
    } else {
        ELOG_ERROR("No appsink in pipeline\n");
    }
    
}

void VideoGstAnalyzer::disconnect(owt_base::InternalOut* out){
    ELOG_DEBUG("Disconnect remote connection\n");
    m_internalout.remove(out);
    g_object_set (G_OBJECT (sink), "emit-signals", FALSE, "sync", FALSE, NULL);
}

int VideoGstAnalyzer::getListeningPort() {
    int listeningPort; 
    listeningPort = m_internalin->getListeningPort();
    ELOG_DEBUG(">>>>>Listen port is :%d\n", listeningPort);
    return listeningPort; 
}

void VideoGstAnalyzer::setOutputParam(std::string codec, int width, int height, 
    int framerate, int bitrate, int kfi, std::string algo, std::string libraryName){

    this->codec = codec;
    this->width = width;
    this->height = height;
    this->framerate = framerate;
    this->bitrate = bitrate;
    this->kfi = kfi;
    this->algo = algo;
    this->libraryName = libraryName;

    std::cout << "setting param,codec=" << this->codec<<",width=" << this->width << ",height="
         << this->height << ",framerate=" << this->framerate<<",bitrate=" << this->bitrate << ",kfi="
         << this->kfi << ",algo=" << this->algo << ",pluginName=" << this->libraryName << std::endl;
}

}
