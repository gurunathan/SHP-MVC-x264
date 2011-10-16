/*****************************************************************************
 * lookahead.c: high-level lookahead functions
 *****************************************************************************
 * Copyright (C) 2010-2011 Avail Media and x264 project
 *
 * Authors: Michael Kazmier <mkazmier@availmedia.com>
 *          Alex Giladi <agiladi@availmedia.com>
 *          Steven Walters <kemuri9@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at licensing@x264.com.
 *****************************************************************************/

/* LOOKAHEAD (threaded and non-threaded mode)
 *
 * Lookahead types:
 *     [1] Slice type / scene cut;
 *
 * In non-threaded mode, we run the existing slicetype decision code as it was.
 * In threaded mode, we run in a separate thread, that lives between the calls
 * to x264_encoder_open() and x264_encoder_close(), and performs lookahead for
 * the number of frames specified in rc_lookahead.  Recommended setting is
 * # of bframes + # of threads.
 */
#include "common/common.h"
#include "analyse.h"

//#define MVC_DEBUG_PRINT

static void x264_lookahead_shift( x264_sync_frame_list_t *dst, x264_sync_frame_list_t *src, int count )
{
    int i = count;
#if defined(MVC_DEBUG_PRINT)
    printf("Lookahead shift = %d\n",count);
#endif
    while( i-- )
    {
        assert( dst->i_size < dst->i_max_size );
        assert( src->i_size );
        dst->list[ dst->i_size++ ] = x264_frame_shift( src->list );
        src->i_size--;
    }
    if( count )
    {
        x264_pthread_cond_broadcast( &dst->cv_fill );
        x264_pthread_cond_broadcast( &src->cv_empty );
    }
}

static void x264_lookahead_update_last_nonb( x264_t *h, x264_frame_t *new_nonb )
{
    if( h->lookahead->last_nonb )
        x264_frame_push_unused( h, h->lookahead->last_nonb );
    h->lookahead->last_nonb = new_nonb;
    new_nonb->i_reference_count++;
}

/*
** Update last non_b in case of right view frames of MVC
*/
static void x264_lookahead_update_last_nonb_dep( x264_t *h, x264_frame_t *new_nonb )
{
    if( h->lookahead->last_nonb_dependent )
        x264_frame_push_unused( h, h->lookahead->last_nonb_dependent );
    h->lookahead->last_nonb_dependent = new_nonb;
    new_nonb->i_reference_count++;
}

#if HAVE_THREAD
static void x264_lookahead_slicetype_decide( x264_t *h )
{
    static int frameNum = 0;
    int b_mvc_flag = h->param.b_mvc_flag;
#if defined(MVC_DEBUG_PRINT)
    printf("LOG: Inside lookahead_slicetype_decide\n");
#endif
    x264_stack_align( x264_slicetype_decide, h );
#ifdef __DEBUG
    x264_log(h, X264_LOG_INFO, "[update last_nonb 0]: frame %d\n", h->lookahead->next.list[0]->i_frame);
#endif
#if defined(MVC_DEBUG_PRINT)
    printf("before look ahead call\n");
#endif
    x264_lookahead_update_last_nonb( h, h->lookahead->next.list[0] );
    if( b_mvc_flag )
        x264_lookahead_update_last_nonb_dep( h, h->lookahead->next_dependent.list[0] );
#if defined(MVC_DEBUG_PRINT)
    printf("after lookahead call\n");
#endif

    // Total number of pics in the lookahead list
    int shift = h->lookahead->next.list[0]->i_bframes + 1;
    x264_pthread_mutex_lock( &h->lookahead->ofbuf.mutex );
    if( !b_mvc_flag )
    {
        while( h->lookahead->ofbuf.i_size == h->lookahead->ofbuf.i_max_size )
        {
            x264_pthread_cond_wait( &h->lookahead->ofbuf.cv_empty, &h->lookahead->ofbuf.mutex );
        }
    }
    else
    { // mvc case
        while( h->lookahead->ofbuf.i_max_size - h->lookahead->ofbuf.i_size < (shift * 2))
        {
            x264_pthread_cond_wait( &h->lookahead->ofbuf.cv_empty, &h->lookahead->ofbuf.mutex );
        }
    }
    x264_pthread_mutex_lock( &h->lookahead->next.mutex );
    x264_pthread_mutex_lock( &h->lookahead->next_dependent.mutex );
    if (!h->param.b_mvc_flag)
    {
      x264_lookahead_shift( &h->lookahead->ofbuf, &h->lookahead->next, shift );
    }
    else
    { // for MVC
        for (int i = 0; i < shift; i++)
        {
            int base_i_type = h->lookahead->next.list[0]->i_type;
            for (int j = 0; j < X264_BFRAME_MAX + 2; j++)
            {
                for (int k = 0; k < X264_BFRAME_MAX + 2; k++)
                {
                    h->lookahead->next_dependent.list[0]->i_cost_est[j][k] = h->lookahead->next.list[0]->i_cost_est[j][k];
                    h->lookahead->next_dependent.list[0]->i_cost_est_aq[j][k] = h->lookahead->next.list[0]->i_cost_est_aq[j][k];
                    h->lookahead->next_dependent.list[0]->i_cpb_duration = h->lookahead->next.list[0]->i_cpb_duration;
                    h->lookahead->next_dependent.list[0]->i_cpb_delay = h->lookahead->next.list[0]->i_cpb_delay;
                    h->lookahead->next_dependent.list[0]->i_dpb_output_delay = h->lookahead->next.list[0]->i_dpb_output_delay;
#ifdef __DEBUG
                    if ( h->lookahead->next.list[0]->i_cost_est[j][k] >= 0) {
                        x264_log(h, X264_LOG_INFO, "i_cost_est[%d][%d] = %d\n", j, k, h->lookahead->next.list[0]->i_cost_est[j][k]);
                    }
#endif
                }
            }

#ifdef __DEBUG
      x264_log(h, X264_LOG_INFO, "base_i_type %d\n", base_i_type);
#endif

            int dep_i_type;
#if 0
            int b_anchor = 0;
#endif
            switch(base_i_type)
            {
#if 0
                case X264_TYPE_I :
                case X264_TYPE_IDR :
                case X264_TYPE_KEYFRAME :
                    dep_i_type = X264_TYPE_ANCHOR;
                    b_anchor = 1;
                break;
#endif
                case X264_TYPE_P :
                    dep_i_type = X264_TYPE_P;
                break;
                case X264_TYPE_B :
                    dep_i_type = X264_TYPE_B;
                break;
                case X264_TYPE_BREF :
                    dep_i_type = X264_TYPE_BREF;
                break;
                default :
                    dep_i_type = X264_TYPE_P;
                break;
            }
            h->lookahead->next_dependent.list[0]->i_type = dep_i_type;
            x264_lookahead_shift( &h->lookahead->ofbuf, &h->lookahead->next, 1 );
            x264_lookahead_shift( &h->lookahead->ofbuf, &h->lookahead->next_dependent, 1 );
        }
    }

    x264_pthread_mutex_unlock( &h->lookahead->next.mutex );
    x264_pthread_mutex_unlock( &h->lookahead->next_dependent.mutex );
    frameNum++;

    /* For MB-tree and VBV lookahead, we have to perform propagation analysis on I-frames too. */
    if( h->lookahead->b_analyse_keyframe && IS_X264_TYPE_I( h->lookahead->last_nonb->i_type ) )
        x264_stack_align( x264_slicetype_analyse, h, 1 );

    x264_pthread_mutex_unlock( &h->lookahead->ofbuf.mutex );
}

static void x264_lookahead_thread( x264_t *h )
{
    int shift;
    int shift_dependent;
    int b_mvc_flag = h->param.b_mvc_flag;
    x264_log( h, X264_LOG_INFO, "b_mvc_flag %d\n", b_mvc_flag);
#if HAVE_MMX
    if( h->param.cpu&X264_CPU_SSE_MISALIGN )
        x264_cpu_mask_misalign_sse();
#endif
    while( !h->lookahead->b_exit_thread )
    {
        x264_pthread_mutex_lock( &h->lookahead->ifbuf.mutex );
        x264_pthread_mutex_lock( &h->lookahead->next.mutex );
        x264_pthread_mutex_lock( &h->lookahead->next_dependent.mutex );
        if( !b_mvc_flag )
        {
            shift = X264_MIN( h->lookahead->next.i_max_size - h->lookahead->next.i_size, h->lookahead->ifbuf.i_size );
            // base and dependent will be divided to the other buffer
            x264_lookahead_shift( &h->lookahead->next, &h->lookahead->ifbuf, shift);
        }
        else
        {
            shift = X264_MIN( h->lookahead->next.i_max_size - h->lookahead->next.i_size, h->lookahead->ifbuf.i_size / 2);
            shift_dependent = X264_MIN( h->lookahead->next_dependent.i_max_size - h->lookahead->next_dependent.i_size,
                                    h->lookahead->ifbuf.i_size / 2);
            shift = X264_MIN(shift, shift_dependent);
            shift_dependent = shift;
#ifdef __DEBUG
            if( h->lookahead->ifbuf.i_size > 1 )
            {
                x264_log( h, X264_LOG_INFO, "i_size %d\n", h->lookahead->ifbuf.i_size);
            }
#endif
#ifdef __DEBUG
            if( (shift > 0) || (shift_dependent > 0) )
            {
                x264_log( h, X264_LOG_INFO, "shift %d shift_dep %d\n", shift, shift_dependent);
            }
#endif
            // base and dependent will be divided to the other buffer
            while( shift > 0 || shift_dependent > 0 )
            {
                if( shift )
                {
                    x264_lookahead_shift( &h->lookahead->next, &h->lookahead->ifbuf, 1);
                    shift--;
                }
                if( shift_dependent )
                {
                    x264_lookahead_shift( &h->lookahead->next_dependent, &h->lookahead->ifbuf, 1 );
                    shift_dependent--;
                }
            }
        } //End of else
        x264_pthread_mutex_unlock( &h->lookahead->next.mutex );
        x264_pthread_mutex_unlock( &h->lookahead->next_dependent.mutex );
        if ( (!b_mvc_flag && (h->lookahead->next.i_size <= h->lookahead->i_slicetype_length + h->param.b_vfr_input)) ||
             (b_mvc_flag && ((h->lookahead->next.i_size <= h->lookahead->i_slicetype_length + h->param.b_vfr_input )
             || (h->lookahead->next_dependent.i_size <= h->lookahead->i_slicetype_length + h->param.b_vfr_input) ) ) )
        {
            while( !h->lookahead->ifbuf.i_size && !h->lookahead->b_exit_thread )
                x264_pthread_cond_wait( &h->lookahead->ifbuf.cv_fill, &h->lookahead->ifbuf.mutex );
            x264_pthread_mutex_unlock( &h->lookahead->ifbuf.mutex );
        }
        else
        {
            x264_pthread_mutex_unlock( &h->lookahead->ifbuf.mutex );
            x264_lookahead_slicetype_decide( h );
        }
    }   /* end of input frames */
    x264_pthread_mutex_lock( &h->lookahead->ifbuf.mutex );
    x264_pthread_mutex_lock( &h->lookahead->next.mutex );
    // For MVC
    x264_pthread_mutex_lock( &h->lookahead->next_dependent.mutex );
    if ( !h->param.b_mvc_flag ) // not mvc
    {
      x264_lookahead_shift( &h->lookahead->next, &h->lookahead->ifbuf, h->lookahead->ifbuf.i_size );
    }
    else // mvc
    {
        for (int i = 0; i < (h->lookahead->ifbuf.i_size / 2); i++) {
            x264_lookahead_shift( &h->lookahead->next, &h->lookahead->ifbuf, 1);
            x264_lookahead_shift( &h->lookahead->next_dependent, &h->lookahead->ifbuf, 1);
        }
    }
    x264_pthread_mutex_unlock( &h->lookahead->next_dependent.mutex );
    x264_pthread_mutex_unlock( &h->lookahead->next.mutex );
    x264_pthread_mutex_unlock( &h->lookahead->ifbuf.mutex );
    while( h->lookahead->next.i_size )
        x264_lookahead_slicetype_decide( h );
    x264_pthread_mutex_lock( &h->lookahead->ofbuf.mutex );
    h->lookahead->b_thread_active = 0;
    x264_pthread_cond_broadcast( &h->lookahead->ofbuf.cv_fill );
    x264_pthread_mutex_unlock( &h->lookahead->ofbuf.mutex );
}
#endif

int x264_lookahead_init( x264_t *h, int i_slicetype_length )
{
    x264_lookahead_t *look;
    CHECKED_MALLOCZERO( look, sizeof(x264_lookahead_t) );
    for( int i = 0; i < h->param.i_threads; i++ )
        h->thread[i]->lookahead = look;

    look->i_last_keyframe = - h->param.i_keyint_max;
    if( h->param.b_mvc_flag )
    {
        look->b_code_anchor_frame = 0;
        look->b_early_termination = 0;
    }
    look->b_analyse_keyframe = (h->param.rc.b_mb_tree || (h->param.rc.i_vbv_buffer_size && h->param.rc.i_lookahead))
                               && !h->param.rc.b_stat_read;
    look->i_slicetype_length = i_slicetype_length;

    /* init frame lists */
    if( x264_sync_frame_list_init( &look->ifbuf, h->param.i_sync_lookahead+3 ) ||
        x264_sync_frame_list_init( &look->next, h->frames.i_delay+3 ) ||
        x264_sync_frame_list_init( &look->ofbuf, (h->frames.i_delay+3) * ( h->param.b_mvc_flag ? 2 : 1) ) )
        goto fail;

    /* for MVC lookahead */
    if (x264_sync_frame_list_init( &look->next_dependent, h->frames.i_delay + 3))
        goto fail;

    if( !h->param.i_sync_lookahead )
        return 0;

    x264_t *look_h = h->thread[h->param.i_threads];
    *look_h = *h;
    if( x264_macroblock_cache_allocate( look_h ) )
        goto fail;

    if( x264_macroblock_thread_allocate( look_h, 1 ) < 0 )
        goto fail;

    if( x264_pthread_create( &look->thread_handle, NULL, (void*)x264_lookahead_thread, look_h ) )
        goto fail;
    look->b_thread_active = 1;

    return 0;
fail:
    x264_free( look );
    return -1;
}

void x264_lookahead_delete( x264_t *h )
{
    if( h->param.i_sync_lookahead )
    {
        x264_pthread_mutex_lock( &h->lookahead->ifbuf.mutex );
        h->lookahead->b_exit_thread = 1;
        x264_pthread_cond_broadcast( &h->lookahead->ifbuf.cv_fill );
        x264_pthread_mutex_unlock( &h->lookahead->ifbuf.mutex );
        x264_pthread_join( h->lookahead->thread_handle, NULL );
        x264_macroblock_cache_free( h->thread[h->param.i_threads] );
        x264_macroblock_thread_free( h->thread[h->param.i_threads], 1 );
        x264_free( h->thread[h->param.i_threads] );
    }
    x264_sync_frame_list_delete( &h->lookahead->ifbuf );
    x264_sync_frame_list_delete( &h->lookahead->next );
    //Lookahead list for dependent~(right) views of MVC
    x264_sync_frame_list_delete( &h->lookahead->next_dependent);
    if( h->lookahead->last_nonb )
        x264_frame_push_unused( h, h->lookahead->last_nonb );
    if( h->param.b_mvc_flag && h->lookahead->last_nonb_dependent )
        x264_frame_push_unused( h, h->lookahead->last_nonb_dependent );
    x264_sync_frame_list_delete( &h->lookahead->ofbuf );
    x264_free( h->lookahead );
}

void x264_lookahead_put_frame( x264_t *h, x264_frame_t *frame )
{
    static int frame_num = 0;
    // if lookahead is enabled, base and dependent is divided at the next function
    if( h->param.i_sync_lookahead )
    {
        x264_sync_frame_list_push( &h->lookahead->ifbuf, frame );
    }
    // if lookahead is disabled, base and dependent is divided here
    else
    {
        if( !h->param.b_mvc_flag ) //AVC path
        {
            x264_sync_frame_list_push( &h->lookahead->next, frame );
        }
        else // mvc case
        {
            if (frame_num & 0x1) {
                x264_sync_frame_list_push( &h->lookahead->next_dependent, frame );
            }
            else
            {
            x264_sync_frame_list_push( &h->lookahead->next, frame );
            }
        }
    }
    frame_num++;
}

int x264_lookahead_is_empty( x264_t *h )
{
    x264_pthread_mutex_lock( &h->lookahead->ofbuf.mutex );
    x264_pthread_mutex_lock( &h->lookahead->next.mutex );
    /*
    ** Lock the mutex of the dependent lookahead mutex (used in MVC)
    */
    x264_pthread_mutex_lock( &h->lookahead->next_dependent.mutex );
    int b_empty = !h->lookahead->next.i_size && !h->lookahead->ofbuf.i_size && !h->lookahead->next_dependent.i_size;
    //Unlock dependent lookahead mutex
    x264_pthread_mutex_unlock( &h->lookahead->next_dependent.mutex );
    x264_pthread_mutex_unlock( &h->lookahead->next.mutex );
    x264_pthread_mutex_unlock( &h->lookahead->ofbuf.mutex );
    return b_empty;
}

static void x264_lookahead_encoder_shift( x264_t *h )
{
    if( !h->lookahead->ofbuf.i_size )
        return;
    int i_frames;
    if( !h->param.b_mvc_flag ) //AVC Path
    {
        i_frames = h->lookahead->ofbuf.list[0]->i_bframes + 1;
    }
    /*
    ** In case of MVC, the number of frames is doubled (Left view + right view)
    */
    else
    {
        i_frames = (h->lookahead->ofbuf.list[0]->i_bframes + 1) * 2;
    }
    while( i_frames-- )
    {
        x264_frame_push( h->frames.current, x264_frame_shift( h->lookahead->ofbuf.list ) );
        h->lookahead->ofbuf.i_size--;
    }
    x264_pthread_cond_broadcast( &h->lookahead->ofbuf.cv_empty );
}

void x264_lookahead_get_frames( x264_t *h )
{
    static int frameNum = 0;
#if defined(MVC_DEBUG_PRINT)
    printf("LOG: Inside lookahead_get_frames\n");
#endif
    if( h->param.i_sync_lookahead )
    {   /* We have a lookahead thread, so get frames from there */
        x264_pthread_mutex_lock( &h->lookahead->ofbuf.mutex );
        if( !h->param.b_mvc_flag ) //AVC Path
        {
            while( !h->lookahead->ofbuf.i_size && h->lookahead->b_thread_active )
                x264_pthread_cond_wait( &h->lookahead->ofbuf.cv_fill, &h->lookahead->ofbuf.mutex );
            x264_lookahead_encoder_shift( h );
        }
        else //MVC case
        {
          if ((h->lookahead->next.i_size + h->lookahead->next_dependent.i_size
            + h->lookahead->ifbuf.i_size + h->lookahead->ofbuf.i_size) >= 2)
            {

                while( (h->lookahead->ofbuf.i_size < 2) && h->lookahead->b_thread_active )
                {
                    x264_pthread_cond_wait( &h->lookahead->ofbuf.cv_fill, &h->lookahead->ofbuf.mutex );
                }
                x264_lookahead_encoder_shift( h );
            }
            else // put frame is only 1. wait for next put frame
            {
                return;
            }
        }
        x264_pthread_mutex_unlock( &h->lookahead->ofbuf.mutex );
        frameNum++;
    }
    else
    {   /* We are not running a lookahead thread, so perform all the slicetype decide on the fly */

        if( h->frames.current[0] || !h->lookahead->next.i_size )
            return;

        x264_stack_align( x264_slicetype_decide, h );
        x264_lookahead_update_last_nonb( h, h->lookahead->next.list[0] );
        x264_lookahead_shift( &h->lookahead->ofbuf, &h->lookahead->next, h->lookahead->next.list[0]->i_bframes + 1 );

        /* For MB-tree and VBV lookahead, we have to perform propagation analysis on I-frames too. */
        if( h->lookahead->b_analyse_keyframe && IS_X264_TYPE_I( h->lookahead->last_nonb->i_type ) )
            x264_stack_align( x264_slicetype_analyse, h, 1 );

        x264_lookahead_encoder_shift( h );
    }
}
