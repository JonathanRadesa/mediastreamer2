/*
mediastreamer2 library - modular sound and video processing and streaming

 * Copyright (C) 2011  Belledonne Communications, Grenoble, France

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include "mediastreamer2/bitratecontrol.h"

#define STATS_HISTORY 3

static const float unacceptable_loss_rate=20;
static const int big_jitter=40; /*ms */
static const float significant_delay=0.2; /*seconds*/
static const int max_ptime=100;

enum state_t{
	Init,
	Probing,
	Stable,
	ProbingUp
};

const char *state_name(enum state_t st){
	switch(st){
		case Init: return "Init";
		case Probing: return "Probing";
		case Stable: return "Stable";
		case ProbingUp: return "ProbingUp";
	}
	return "bad state";
}

typedef struct rtpstats{
	uint64_t high_seq_recv; /*highest sequence number received*/
	float lost_percentage; /*percentage of lost packet since last report*/
	float int_jitter; /*interrarrival jitter */
	float rt_prop; /*round trip propagation*/
}rtpstats_t;

enum action_type{
	DoNothing,
	DecreaseBitrate,
	DecreasePacketRate,
	IncreaseQuality
};

static const char *action_type_name(enum action_type t){
	switch(t){
		case DoNothing:
			return "DoNothing";
		case IncreaseQuality:
			return "IncreaseQuality";
		case DecreaseBitrate:
			return "DecreaseBitrate";
		case DecreasePacketRate:
			return "DecreasePacketRate";
	}
	return "bad action type";
}

typedef struct action{
	enum action_type type;
	int value;
}action_t;

struct _MSAudioBitrateController{
	RtpSession *session;
	MSFilter *encoder;
	int clockrate;
	rtpstats_t stats[STATS_HISTORY];
	int curindex;
	enum state_t state;
	int min_ptime;
	int nom_bitrate;
	int cur_ptime;
	int cur_bitrate;
	int stable_count;
};

MSAudioBitrateController *ms_audio_bitrate_controller_new(RtpSession *session, MSFilter *encoder, unsigned int flags){
	MSAudioBitrateController *rc=ms_new0(MSAudioBitrateController,1);
	rc->session=session;
	rc->encoder=encoder;
	rc->cur_ptime=rc->min_ptime=20;
	rc->cur_bitrate=rc->nom_bitrate=0;
	if (ms_filter_call_method(encoder,MS_FILTER_GET_BITRATE,&rc->nom_bitrate)!=0){
		ms_message("Encoder has nominal bitrate %i",rc->nom_bitrate);
	}	
	rc->cur_bitrate=rc->nom_bitrate;
	return rc;
}

static bool_t rt_prop_doubled(rtpstats_t *cur,rtpstats_t *prev){
	//ms_message("AudioBitrateController: cur=%f, prev=%f",cur->rt_prop,prev->rt_prop);
	if (cur->rt_prop>=significant_delay && prev->rt_prop>0){	
		if (cur->rt_prop>=(prev->rt_prop*2.0)){
			/*propagation doubled since last report */
			return TRUE;
		}
	}
	return FALSE;
}

static bool_t rt_prop_increased(MSAudioBitrateController *obj){
	rtpstats_t *cur=&obj->stats[obj->curindex % STATS_HISTORY];
	rtpstats_t *prev=&obj->stats[(STATS_HISTORY+obj->curindex-1) % STATS_HISTORY];

	if (rt_prop_doubled(cur,prev)){
		return TRUE;
	}
	return FALSE;
}

static void analyse_quality(MSAudioBitrateController *obj, action_t *action){
	rtpstats_t *cur=&obj->stats[obj->curindex % STATS_HISTORY];
	/*big losses and big jitter */
	if (cur->lost_percentage>=unacceptable_loss_rate && cur->int_jitter>=big_jitter){
		action->type=DecreaseBitrate;
		action->value=MIN(cur->lost_percentage,50);
		ms_message("AudioBitrateController: analyse - loss rate unacceptable and big jitter");
	}else if (rt_prop_increased(obj)){
		action->type=DecreaseBitrate;
		action->value=20;
		ms_message("AudioBitrateController: analyse - rt_prop doubled.");
	}else if (cur->lost_percentage>=unacceptable_loss_rate){
		/*big loss rate but no jitter, and no big rtp_prop: pure lossy network*/
		action->type=DecreasePacketRate;
		ms_message("AudioBitrateController: analyse - loss rate unacceptable.");
	}else{
		action->type=DoNothing;
		ms_message("AudioBitrateController: analyse - everything is fine.");
	}
}

static bool_t has_improved(MSAudioBitrateController *obj){
	rtpstats_t *cur=&obj->stats[obj->curindex % STATS_HISTORY];
	rtpstats_t *prev=&obj->stats[(STATS_HISTORY+obj->curindex-1) % STATS_HISTORY];
	rtpstats_t *prev2=&obj->stats[(STATS_HISTORY+obj->curindex-2) % STATS_HISTORY];

	if (prev->lost_percentage>=unacceptable_loss_rate){
		if (cur->lost_percentage<prev->lost_percentage){
			ms_message("AudioBitrateController: lost percentage has improved");
			return TRUE;
		}else goto end;
	}
	if (rt_prop_doubled(prev,prev2) && cur->rt_prop<prev->rt_prop){
		ms_message("AudioBitrateController: rt prop decrased");
		return TRUE;
	}

end:
	ms_message("AudioBitrateController: no improvements.");
	
	return FALSE;
}

static void apply_ptime(MSAudioBitrateController *obj){
	char tmp[64];
	snprintf(tmp,sizeof(tmp),"ptime=%i",obj->cur_ptime);
	if (ms_filter_call_method(obj->encoder,MS_FILTER_ADD_FMTP,tmp)!=0){
		ms_message("AudioBitrateController: failed ptime command.");
	}else ms_message("AudioBitrateController: ptime changed to %i",obj->cur_ptime);
}

static void inc_ptime(MSAudioBitrateController *obj){
	if (obj->cur_ptime>=max_ptime){
		ms_message("AudioBitrateController: maximum ptime reached");
		return;
	}
	obj->cur_ptime+=obj->min_ptime;
	apply_ptime(obj);
}

static int execute_action(MSAudioBitrateController *obj, action_t *action){
	ms_message("AudioBitrateController: executing action of type %s, value=%i",action_type_name(action->type),action->value);
	if (action->type==DecreaseBitrate){
		if (obj->nom_bitrate==0){
			/*not a vbr codec*/
			inc_ptime(obj);
		}else{
			int cur_br=0;
			int new_br;
			if (ms_filter_call_method(obj->encoder,MS_FILTER_GET_BITRATE,&cur_br)!=0){
				ms_message("AudioBitrateController: GET_BITRATE failed");
				inc_ptime(obj);
				return 0;
			}
			new_br=cur_br-((cur_br*action->value)/100);
		
			ms_message("AudioBitrateController: Attempting to reduce audio bitrate to %i",new_br);
			if (ms_filter_call_method(obj->encoder,MS_FILTER_SET_BITRATE,&new_br)!=0){
				ms_message("AudioBitrateController: SET_BITRATE failed");
				inc_ptime(obj);
				return 0;
			}
			new_br=0;
			ms_filter_call_method(obj->encoder,MS_FILTER_GET_BITRATE,&new_br);
			ms_message("AudioBitrateController: bitrate actually set to %i");
		}
	}else if (action->type==DecreasePacketRate){
		inc_ptime(obj);
	}else if (action->type==IncreaseQuality){
		if (obj->cur_ptime>obj->min_ptime){
			obj->cur_ptime-=obj->min_ptime;
			apply_ptime(obj);
		}else return -1;
	}
	return 0;
}

static void state_machine(MSAudioBitrateController *obj){
	action_t action;
	switch(obj->state){
		case Stable:
			obj->stable_count++;
		case Init:
			analyse_quality(obj,&action);
			if (action.type!=DoNothing){
				execute_action(obj,&action);
				obj->state=Probing;
			}else if (obj->stable_count>=5){
				action.type=IncreaseQuality;
				execute_action(obj,&action);
				obj->state=ProbingUp;
			}
		break;
		case Probing:
			obj->stable_count=0;
			if (has_improved(obj)){
				obj->state=Stable;
			}else{
				analyse_quality(obj,&action);
				if (action.type!=DoNothing){
					execute_action(obj,&action);
				}
			}
		break;
		case ProbingUp:
			obj->stable_count=0;
			analyse_quality(obj,&action);
			if (action.type!=DoNothing){
				execute_action(obj,&action);
				obj->state=Probing;
			}else{
				/*continue*/
				action.type=IncreaseQuality;
				if (execute_action(obj,&action)==-1){
					/* we reached the maximum*/
					obj->state=Init;
				}
			}
		break;
		default:
		break;
	}
	ms_message("AudioBitrateController: current state is %s",state_name(obj->state));
}

static void read_report(MSAudioBitrateController *obj, const report_block_t *rb){
	rtpstats_t *cur;

	obj->curindex++;
	cur=&obj->stats[obj->curindex % STATS_HISTORY];
	
	if (obj->clockrate==0){
		PayloadType *pt=rtp_profile_get_payload(rtp_session_get_send_profile(obj->session),rtp_session_get_send_payload_type(obj->session));
		if (pt!=NULL) obj->clockrate=pt->clock_rate;
		else return;
	}
	
	cur->high_seq_recv=report_block_get_high_ext_seq(rb);
	cur->lost_percentage=100.0*(float)report_block_get_fraction_lost(rb)/256.0;
	cur->int_jitter=1000.0*(float)report_block_get_interarrival_jitter(rb)/(float)obj->clockrate;
	cur->rt_prop=rtp_session_get_round_trip_propagation(obj->session);
	ms_message("AudioBitrateController: lost_percentage=%f, int_jitter=%f ms, rt_prop=%f sec",cur->lost_percentage,cur->int_jitter,cur->rt_prop);
}

void ms_audio_bitrate_controller_process_rtcp(MSAudioBitrateController *obj, mblk_t *rtcp){
	const report_block_t *rb=NULL;
	if (rtcp_is_SR(rtcp)){
		rb=rtcp_SR_get_report_block(rtcp,0);
	}else if (rtcp_is_RR(rtcp)){
		rb=rtcp_RR_get_report_block(rtcp,0);
	}
	if (rb){
		read_report(obj,rb);
		state_machine(obj);
	}
}

void ms_audio_bitrate_controller_destroy(MSAudioBitrateController *obj){
	ms_free(obj);
}

