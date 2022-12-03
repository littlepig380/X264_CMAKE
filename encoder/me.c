/*****************************************************************************
 * me.c: motion estimation
 *****************************************************************************
 * Copyright (C) 2003-2022 x264 project
 *
 * Authors: Loren Merritt <lorenm@u.washington.edu>
 *          Laurent Aimar <fenrir@via.ecp.fr>
 *          Fiona Glaser <fiona@x264.com>
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

#include "common/common.h"
#include "macroblock.h"
#include "me.h"

/* presets selected from good points on the speed-vs-quality curve of several test videos
 * subpel_iters[i_subpel_refine] = { refine_hpel, refine_qpel, me_hpel, me_qpel }
 * where me_* are the number of EPZS iterations run on all candidate block types,
 * and refine_* are run only on the winner.
 * the subme=8,9 values are much higher because any amount of satd search makes
 * up its time by reducing the number of qpel-rd iterations. */
static const uint8_t subpel_iterations[][4] =
   {{0,0,0,0},
    {1,1,0,0},
    {0,1,1,0},
    {0,2,1,0},
    {0,2,1,1},
    {0,2,1,2},
    {0,0,2,2},
    {0,0,2,2},
    {0,0,4,10},
    {0,0,4,10},
    {0,0,4,10},
    {0,0,4,10}};

/* (x-1)%6 */
static const uint8_t mod6m1[8] = {5,0,1,2,3,4,5,0};
/* radius 2 hexagon. repeated entries are to avoid having to compute mod6 every time. */
static const int8_t hex2[8][2] = {{-1,-2}, {-2,0}, {-1,2}, {1,2}, {2,0}, {1,-2}, {-1,-2}, {-2,0}};
static const int8_t square1[9][2] = {{0,0}, {0,-1}, {0,1}, {-1,0}, {1,0}, {-1,-1}, {-1,1}, {1,-1}, {1,1}};

static void refine_subpel( x264_t *h, x264_me_t *m, int hpel_iters, int qpel_iters, int *p_halfpel_thresh, int b_refine_qpel );

#define BITS_MVD( mx, my )\
    (p_cost_mvx[(mx)*4] + p_cost_mvy[(my)*4])

#define COST_MV( mx, my )\
do\
{\
    int cost = h->pixf.fpelcmp[i_pixel]( p_fenc, FENC_STRIDE,\
                   &p_fref_w[(my)*stride+(mx)], stride )\
             + BITS_MVD(mx,my);\
    COPY3_IF_LT( bcost, cost, bmx, mx, bmy, my );\
} while( 0 )

#define COST_MV_HPEL( mx, my, cost )\
do\
{\
    intptr_t stride2 = 16;\
    pixel *src = h->mc.get_ref( pix, &stride2, m->p_fref, stride, mx, my, bw, bh, &m->weight[0] );\
    cost = h->pixf.fpelcmp[i_pixel]( p_fenc, FENC_STRIDE, src, stride2 )\
         + p_cost_mvx[ mx ] + p_cost_mvy[ my ];\
} while( 0 )

#define COST_MV_X3_DIR( m0x, m0y, m1x, m1y, m2x, m2y, costs )\
{\
    pixel *pix_base = p_fref_w + bmx + bmy*stride;\
    h->pixf.fpelcmp_x3[i_pixel]( p_fenc,\
        pix_base + (m0x) + (m0y)*stride,\
        pix_base + (m1x) + (m1y)*stride,\
        pix_base + (m2x) + (m2y)*stride,\
        stride, costs );\
    (costs)[0] += BITS_MVD( bmx+(m0x), bmy+(m0y) );\
    (costs)[1] += BITS_MVD( bmx+(m1x), bmy+(m1y) );\
    (costs)[2] += BITS_MVD( bmx+(m2x), bmy+(m2y) );\
}

#define COST_MV_X4_DIR( m0x, m0y, m1x, m1y, m2x, m2y, m3x, m3y, costs )\
{\
    pixel *pix_base = p_fref_w + bmx + bmy*stride;\
    h->pixf.fpelcmp_x4[i_pixel]( p_fenc,\
        pix_base + (m0x) + (m0y)*stride,\
        pix_base + (m1x) + (m1y)*stride,\
        pix_base + (m2x) + (m2y)*stride,\
        pix_base + (m3x) + (m3y)*stride,\
        stride, costs );\
    (costs)[0] += BITS_MVD( bmx+(m0x), bmy+(m0y) );\
    (costs)[1] += BITS_MVD( bmx+(m1x), bmy+(m1y) );\
    (costs)[2] += BITS_MVD( bmx+(m2x), bmy+(m2y) );\
    (costs)[3] += BITS_MVD( bmx+(m3x), bmy+(m3y) );\
}

#define COST_MV_X4( m0x, m0y, m1x, m1y, m2x, m2y, m3x, m3y )\
{\
    pixel *pix_base = p_fref_w + omx + omy*stride;\
    h->pixf.fpelcmp_x4[i_pixel]( p_fenc,\
        pix_base + (m0x) + (m0y)*stride,\
        pix_base + (m1x) + (m1y)*stride,\
        pix_base + (m2x) + (m2y)*stride,\
        pix_base + (m3x) + (m3y)*stride,\
        stride, costs );\
    costs[0] += BITS_MVD( omx+(m0x), omy+(m0y) );\
    costs[1] += BITS_MVD( omx+(m1x), omy+(m1y) );\
    costs[2] += BITS_MVD( omx+(m2x), omy+(m2y) );\
    costs[3] += BITS_MVD( omx+(m3x), omy+(m3y) );\
    COPY3_IF_LT( bcost, costs[0], bmx, omx+(m0x), bmy, omy+(m0y) );\
    COPY3_IF_LT( bcost, costs[1], bmx, omx+(m1x), bmy, omy+(m1y) );\
    COPY3_IF_LT( bcost, costs[2], bmx, omx+(m2x), bmy, omy+(m2y) );\
    COPY3_IF_LT( bcost, costs[3], bmx, omx+(m3x), bmy, omy+(m3y) );\
}

#define COST_MV_X3_ABS( m0x, m0y, m1x, m1y, m2x, m2y )\
{\
    h->pixf.fpelcmp_x3[i_pixel]( p_fenc,\
        p_fref_w + (m0x) + (m0y)*stride,\
        p_fref_w + (m1x) + (m1y)*stride,\
        p_fref_w + (m2x) + (m2y)*stride,\
        stride, costs );\
    costs[0] += p_cost_mvx[(m0x)*4]; /* no cost_mvy */\
    costs[1] += p_cost_mvx[(m1x)*4];\
    costs[2] += p_cost_mvx[(m2x)*4];\
    COPY3_IF_LT( bcost, costs[0], bmx, m0x, bmy, m0y );\
    COPY3_IF_LT( bcost, costs[1], bmx, m1x, bmy, m1y );\
    COPY3_IF_LT( bcost, costs[2], bmx, m2x, bmy, m2y );\
}

/*  1  */
/* 101 */
/*  1  */
#define DIA1_ITER( mx, my )\
{\
    omx = mx; omy = my;\
    COST_MV_X4( 0,-1, 0,1, -1,0, 1,0 );\
}

#define CROSS( start, x_max, y_max )\
{\
    int i = start;\
    if( (x_max) <= X264_MIN(mv_x_max-omx, omx-mv_x_min) )\
        for( ; i < (x_max)-2; i+=4 )\
            COST_MV_X4( i,0, -i,0, i+2,0, -i-2,0 );\
    for( ; i < (x_max); i+=2 )\
    {\
        if( omx+i <= mv_x_max )\
            COST_MV( omx+i, omy );\
        if( omx-i >= mv_x_min )\
            COST_MV( omx-i, omy );\
    }\
    i = start;\
    if( (y_max) <= X264_MIN(mv_y_max-omy, omy-mv_y_min) )\
        for( ; i < (y_max)-2; i+=4 )\
            COST_MV_X4( 0,i, 0,-i, 0,i+2, 0,-i-2 );\
    for( ; i < (y_max); i+=2 )\
    {\
        if( omy+i <= mv_y_max )\
            COST_MV( omx, omy+i );\
        if( omy-i >= mv_y_min )\
            COST_MV( omx, omy-i );\
    }\
}

#define FPEL(mv) (((mv)+2)>>2) /* Convert subpel MV to fullpel with rounding... */
#define SPEL(mv) ((mv)*4)      /* ... and the reverse. */
#define SPELx2(mv) (SPEL(mv)&0xFFFCFFFC) /* for two packed MVs */

// 解析参考链接:https://blog.csdn.net/fanbird2008/article/details/30075187
void x264_me_search_ref( x264_t *h, x264_me_t *m, int16_t (*mvc)[2], int i_mvc, int *p_halfpel_thresh )
{
    const int bw = x264_pixel_size[m->i_pixel].w;
    const int bh = x264_pixel_size[m->i_pixel].h;
    const int i_pixel = m->i_pixel;
    const int stride = m->i_stride[0];
    int i_me_range = h->param.analyse.i_me_range;
    int bmx, bmy, bcost = COST_MAX;
    int bpred_cost = COST_MAX;
    int omx, omy, pmx, pmy;
    pixel *p_fenc = m->p_fenc[0];// 指向待编码帧的Y平面(亮度)
    pixel *p_fref_w = m->p_fref_w;// 指向参考帧的的Y平面(亮度)

    // 声明堆栈变量 pixel pix[16 * 16], 并且32字节对齐
    ALIGNED_ARRAY_32( pixel, pix,[16*16] );

    // 声明堆栈变量 int16_t mvc_temp[16][2], 并且8字节对齐
    ALIGNED_ARRAY_8( int16_t, mvc_temp,[16],[2] );

    // 声明堆栈变量 int costs[16], 并且16字节对齐
    ALIGNED_ARRAY_16( int, costs,[16] );

    // mv_limit_fpel 是一个 2x2 的 数组, 存放着整像素运动矢量搜索范围, min_x, min_y, max_x, max_y
    int mv_x_min = h->mb.mv_limit_fpel[0][0];
    int mv_y_min = h->mb.mv_limit_fpel[0][1];
    int mv_x_max = h->mb.mv_limit_fpel[1][0];
    int mv_y_max = h->mb.mv_limit_fpel[1][1];

// 首先定义两个用来检查mv是否超出范围的宏
/* Special version of pack to allow shortcuts in CHECK_MVRANGE */
// pack16to32_mask2 主要是将两个整数分别放入高16位和低16位, 组合成一个32位的整数
#define pack16to32_mask2(mx,my) (((uint32_t)(mx)<<16)|((uint32_t)(my)&0x7FFF))
    uint32_t mv_min = pack16to32_mask2( -mv_x_min, -mv_y_min );
    uint32_t mv_max = pack16to32_mask2( mv_x_max, mv_y_max )|0x8000;
    uint32_t pmv, bpred_mv = 0;

// CHECK_MVRANGE这个宏主要是检测mv有没有超出边界范围,如果超出则不为0
// 这个宏很巧妙, 它将检查mx, my 是否超过了搜索范围
// 具体是, 如果mx, my有一个溢出, 其 or 运算的两边至少有一个的高位为1
// 从而将导致其or的结果与0x80004000进行与运算得到的结果不为0
#define CHECK_MVRANGE(mx,my) (!(((pack16to32_mask2(mx,my) + mv_min) | (mv_max - pack16to32_mask2(mx,my))) & 0x80004000))

    // a->p_cost_mv = h->cost_mv[a->i_qp] 见 x264_mb_analyse_load_costs
    // h->cost_mv 初始化, 见 x264_analyse_init_costs
    // m->p_cost_mv = a->p_cost_mv 见宏定义 LOAD_FUNC
    // [question]这个相减还真有点费解, 一个是地址, 一个是预测运动向量
    const uint16_t *p_cost_mvx = m->p_cost_mv - m->mvp[0];
    const uint16_t *p_cost_mvy = m->p_cost_mv - m->mvp[1];

    /* Try extra predictors if provided.  If subme >= 3, check subpel predictors,
     * otherwise round them to fullpel. */

    // 详细参数解析:https://zhuanlan.zhihu.com/p/473593903
    // i_subpel_refine是外部输入参数subme的代码化变量,是用来控制编码策略的
    // 不过从x264源码里面的实现来看,它不仅可以设置子像素运动估计过程的计算复杂度,
    // 还可以决定编码器整个参数选择过程的复杂度,是否使用RDO,从而影响x264视频编码器的压缩性能和编码速度.
    // 目前subme可配置的取值范围[0,11],默认值是7,对应medium preset.
    // 按照官方的代码注释信息来看,subme的值具体会影响到x264编码器的运动估计和模式决策以及QP决策,
    // 而这三个模块正好是视频编码器算法优化的关键所在,也是RDO率失真优化过程应用最多的模块.
    // (注:此处RDO指的是用原始和重建像素块SSD作为失真指标)

    // i_subpel_refine动态预测和分区方式,可选项1~7,默认5(与压缩质量和时间关系密切,1是7速度的四倍以上)
    // 1:用全像素块进行动态搜索,对每个块再用快速模式进行四分之一像素块精确搜索
    // 2:用半像素块进行动态搜索,对每个块再用快速模式进行四分之一像素块精确搜索
    // 3:用半像素块进行动态搜索,对每个块再用质量模式进行四分之一像素块精确搜索
    // 4:用快速模式进行四分之一像素块精确搜索
    // 5:用质量模式进行四分之一像素块精确搜索
    // 6:进行I、P帧像素块的速率失真最优化(rdo)
    // 7:进行I、P帧运动矢量及块内部的速率失真最优化(质量最好)
    // 8~9:会使用RDO进行最佳MV的搜索.
    // 10:会对QP进行RDO搜索确定最佳值.

    if( h->mb.i_subpel_refine >= 3 )
    {
        // 首先是半像素的运动搜索

        // 第一优先级是计算mvp方向

        // 需要进行 1/4 像素插值
        /* Calculate and check the MVP first */
       
        // SPEL 是半像素 FPEL是全像素
        // SPEL 将整像素坐标转换为 1/4 像素坐标
        // 一个整数的低两位表示1/4像素坐标
        // SPEL 将实参左移两位, 即将原来的坐标乘以 4,
        // 比如：0 1 两个相邻的像素变成了0, 4, 其中空出来的1, 2, 3
        // 就对应着 1/4  1/2  3/4 像素位
        // 而 FPEL 将实参右移两位, 获得整像素位坐标
        // 即将原来放大了的整像素坐标还原
        // 从下面的程序来看, mvp保存的是1/4像素坐标
        int bpred_mx = x264_clip3( m->mvp[0], SPEL(mv_x_min), SPEL(mv_x_max) );
        int bpred_my = x264_clip3( m->mvp[1], SPEL(mv_y_min), SPEL(mv_y_max) );
        pmv = pack16to32_mask( bpred_mx, bpred_my );

        //起点加上mvp得到目标宏块为之后后, 将预测目标宏块的坐标在转化为整像素坐标
        pmx = FPEL( bpred_mx );
        pmy = FPEL( bpred_my );

        // 这个COST_MV_HPEL宏分为两个关键函数
        // get_ref()负责根据mv找到亚像素数据并返回
        // fpelcmp()负责像素级比较计算获得相对应的cost, cost采用求编码宏块和参考宏块整像素的SAD（绝对差之和）
        COST_MV_HPEL( bpred_mx, bpred_my, bpred_cost );

        // 将得到的预测运动向量的代价赋值给pmv_cost
        int pmv_cost = bpred_cost;

        // 第二优先级是计算mvc方向上cost,如果小就替代墙面pmv_cost
        if( i_mvc > 0 )
        {
            // 下面的英文注释已经表达的很清楚了,这翻译一下
            // 裁减候选的运动向量,除掉那些 0 向量 或 等于 pmv (mvp) 的向量 因为mvp已经计算过了
            // 并将裁减结果放在mvc_temp[2]及之后处
            /* Clip MV candidates and eliminate those equal to zero and pmv. */
            int valid_mvcs = x264_predictor_clip( mvc_temp+2, mvc, i_mvc, h->mb.mv_limit_fpel, pmv );
            // 对其他候选向量进行处理
            if( valid_mvcs > 0 )
            {
                int i = 1, cost;
                /* We stuff pmv here to branchlessly pick between pmv and the various
                 * MV candidates. [0] gets skipped in order to maintain alignment for
                 * x264_predictor_clip. */

                // mvc_temp[0] 空着, 为了维护对齐
                // mvc_temp[1] 存放 pmv

                M32( mvc_temp[1] ) = pmv;

                //计算每一个mvc的cost,找到最佳的
                bpred_cost <<= 4;
                do
                {
                    int mx = mvc_temp[i+1][0];
                    int my = mvc_temp[i+1][1];
                    COST_MV_HPEL( mx, my, cost );
                    COPY1_IF_LT( bpred_cost, (cost << 4) + i );
                } while( ++i <= valid_mvcs );

                // 确定最终的预测运动向量
                bpred_mx = mvc_temp[(bpred_cost&15)+1][0];
                bpred_my = mvc_temp[(bpred_cost&15)+1][1];
                bpred_cost >>= 4;
            }
        }

        /* Round the best predictor back to fullpel and get the cost, since this is where
         * we'll be starting the fullpel motion search. */
        // 从1/4像素坐标得到整像素坐标,因为我们将开始整像素运动搜索
        bmx = FPEL( bpred_mx );
        bmy = FPEL( bpred_my );
        bpred_mv = pack16to32_mask(bpred_mx, bpred_my);
        // 预测运动向量是 1/4 像素坐标,即是1/4像素预测(包括1/4, 2/4, 3/4)
        if( bpred_mv&0x00030003 ) /* Only test if the tested predictor is actually subpel... */
            COST_MV( bmx, bmy );  // call x264_pixel_sad_16x16 计算预测运动向量代价
        else                          /* Otherwise just copy the cost (we already know it) */
            bcost = bpred_cost;

        //这里还需要考虑(0,0)向量的情况,这种情况默认被包含在需要检查的范围之内
        /* Test the zero vector if it hasn't been tested yet. */
        if( pmv )// 如果 pmv 不是 0 向量
        {
            // 如果 bmx, bmy也不是0向量        
            // call x264_pixel_sad_16x16 以0向量为预测运动向量计算SAD,
            // 如果代价更小,就替换代价和相应的预测运动向量
            if( bmx|bmy ) COST_MV( 0, 0 );
        }
        /* If a subpel mv candidate was better than the zero vector, the previous
         * fullpel check won't have gotten it even if the pmv was zero. So handle
         * that possibility here. */
        else
        {
            COPY3_IF_LT( bcost, pmv_cost, bmx, 0, bmy, 0 );
        }
    }
    else
    {
        // 这里是由上述h->mb.i_subpel_refine决定, 如果运行参数配置为整像素的探测就走一下逻辑
        // 同样, 首先从mvp开始计算和检查整像素 MVP, 并开始相应的cost计算, 之后则是mvc
        /* Calculate and check the fullpel MVP first */
        bmx = pmx = x264_clip3( FPEL(m->mvp[0]), mv_x_min, mv_x_max );
        bmy = pmy = x264_clip3( FPEL(m->mvp[1]), mv_y_min, mv_y_max );
        pmv = pack16to32_mask( bmx, bmy );

        /* Because we are rounding the predicted motion vector to fullpel, there will be
         * an extra MV cost in 15 out of 16 cases.  However, when the predicted MV is
         * chosen as the best predictor, it is often the case that the subpel search will
         * result in a vector at or next to the predicted motion vector.  Therefore, we omit
         * the cost of the MV from the rounded MVP to avoid unfairly biasing against use of
         * the predicted motion vector.
         *
         * Disclaimer: this is a post-hoc rationalization for why this hack works. */
        // call x264_pixel_sad_16x16 计算 SAD, 以预测运动向量(bmy, bmx)
        bcost = h->pixf.fpelcmp[i_pixel]( p_fenc, FENC_STRIDE, &p_fref_w[bmy*stride+bmx], stride );

        //然后是检查mvc数组中的向量
        if( i_mvc > 0 )
        {
            /* Like in subme>=3, except we also round the candidates to fullpel. */
            int valid_mvcs = x264_predictor_roundclip( mvc_temp+2, mvc, i_mvc, h->mb.mv_limit_fpel, pmv );
            if( valid_mvcs > 0 )
            {
                int i = 1, cost;
                M32( mvc_temp[1] ) = pmv;
                bcost <<= 4;
                do
                {
                    int mx = mvc_temp[i+1][0];
                    int my = mvc_temp[i+1][1];
                    cost = h->pixf.fpelcmp[i_pixel]( p_fenc, FENC_STRIDE, &p_fref_w[my*stride+mx], stride ) + BITS_MVD( mx, my );
                    COPY1_IF_LT( bcost, (cost << 4) + i );
                } while( ++i <= valid_mvcs );
                bmx = mvc_temp[(bcost&15)+1][0];
                bmy = mvc_temp[(bcost&15)+1][1];
                bcost >>= 4;
            }
        }

        /* Same as above, except the condition is simpler. */
        // call x264_pixel_sad_16x16 以0向量为预测运动向量计算SAD,
        // 如果代价更小,就替换代价和相应的预测运动向量
        if( pmv )
            COST_MV( 0, 0 );
    }

    // 候选的mvp与mvc都计算完成之后, 进行邻域遍历式搜索
    // 根据设定的运动估计方法, 来执行搜索
    switch( h->mb.i_me_method )
    {
        // （1）菱形搜索算法（DIA）
        // 以搜索起点为中心,采用下图所示的小菱形模板（模板半径为1）搜索.
        // 计算各点的匹配误差,得到MBD（最小误差）点.
        // 如果MBD点在模板中心,则搜索结束,此时的MBD 点就是最优匹配点,
        // 对应的像素块就是最佳匹配块；如果MBD点不在模板中心位置,
        // 则以现在MBD点为中心点,继续进行小菱形搜索,直至MBD点落在中心点为止.
        case X264_ME_DIA: //菱形型搜索
        {
            /* diamond search, radius 1 */
            bcost <<= 4;
            int i = i_me_range;
            do
            {
                COST_MV_X4_DIR( 0,-1, 0,1, -1,0, 1,0, costs );
                COPY1_IF_LT( bcost, (costs[0]<<4)+1 );
                COPY1_IF_LT( bcost, (costs[1]<<4)+3 );
                COPY1_IF_LT( bcost, (costs[2]<<4)+4 );
                COPY1_IF_LT( bcost, (costs[3]<<4)+12 );
                if( !(bcost&15) )
                    break;
                bmx -= (int32_t)((uint32_t)bcost<<28)>>30;
                bmy -= (int32_t)((uint32_t)bcost<<30)>>30;
                bcost &= ~15;
            } while( --i && CHECK_MVRANGE(bmx, bmy) );
            bcost >>= 4;
            break;
        }
        // （2）六边形搜索算法（HEX）
        // 该方法采用1个大模板（六边形模板）和2个小模板（小菱形模板和小正方形模板）.
        // 具体的搜索步骤如下：
        // 步骤1：以搜索起点为中心,采用图中左边的六边形模板进行搜索.
        // 计算区域中心及周围6个点处的匹配误差并比较,如最小MBD 点位于模板中心点,
        // 则转至步骤2；否则以上一次的MBD 点作为中心点,以六边形模板为模板进行反复搜索.
        // 步骤2：以上一次的MBD 点为中心点,采用小菱形模板搜索,
        // 计算各点的匹配误差,找到MBD 点.然后以MBD点为中心点,
        // 采用小正方形模板搜索,得到的MBD点就是最优匹配点.
        case X264_ME_HEX: //六边形搜索
        {
    me_hex2:
            /* hexagon search, radius 2 */
    #if 0
            for( int i = 0; i < i_me_range/2; i++ )
            {
                omx = bmx; omy = bmy;
                COST_MV( omx-2, omy   );
                COST_MV( omx-1, omy+2 );
                COST_MV( omx+1, omy+2 );
                COST_MV( omx+2, omy   );
                COST_MV( omx+1, omy-2 );
                COST_MV( omx-1, omy-2 );
                if( bmx == omx && bmy == omy )
                    break;
                if( !CHECK_MVRANGE(bmx, bmy) )
                    break;
            }
    #else
            /* equivalent to the above, but eliminates duplicate candidates */

            /* hexagon */
            // call x264_pixel_sad_x3_16x16 分别计算三个点
            // 分别为预测运动向量时的代价
            COST_MV_X3_DIR( -2,0, -1, 2,  1, 2, costs   );
            // 计算另三个点为预测运动向量时的代价
            COST_MV_X3_DIR(  2,0,  1,-2, -1,-2, costs+4 ); /* +4 for 16-byte alignment */
            bcost <<= 3;
            // 如果有代价更小的,替换原来的代价和相应的预测运动向量
            COPY1_IF_LT( bcost, (costs[0]<<3)+2 );
            COPY1_IF_LT( bcost, (costs[1]<<3)+3 );
            COPY1_IF_LT( bcost, (costs[2]<<3)+4 );
            COPY1_IF_LT( bcost, (costs[4]<<3)+5 );
            COPY1_IF_LT( bcost, (costs[5]<<3)+6 );
            COPY1_IF_LT( bcost, (costs[6]<<3)+7 );

            if( bcost&7 )
            {
                // bcost & 7 为真, 说明存在更小的预测运动向量
                // 获取是哪个预测点
                int dir = (bcost&7)-2;
                // 将bmx, bmy 沿这个方向前进（移动）
                bmx += hex2[dir+1][0];
                bmy += hex2[dir+1][1];

                /* half hexagon, not overlapping the previous iteration */
                // 判断是否越界(超过搜索范围)
                for( int i = (i_me_range>>1) - 1; i > 0 && CHECK_MVRANGE(bmx, bmy); i-- )
                {
                    // 仅仅对三个点进行预测求SAD代价,
                    // 因为另外的三个点已经预测求SAD代价过了
                    COST_MV_X3_DIR( hex2[dir+0][0], hex2[dir+0][1],
                                    hex2[dir+1][0], hex2[dir+1][1],
                                    hex2[dir+2][0], hex2[dir+2][1],
                                    costs );
                    bcost &= ~7;
                    COPY1_IF_LT( bcost, (costs[0]<<3)+1 );
                    COPY1_IF_LT( bcost, (costs[1]<<3)+2 );
                    COPY1_IF_LT( bcost, (costs[2]<<3)+3 );
                    if( !(bcost&7) )
                        break;
                    dir += (bcost&7)-2;
                    dir = mod6m1[dir+1];
                    bmx += hex2[dir+1][0];
                    bmy += hex2[dir+1][1];
                }
            }
            bcost >>= 3;
    #endif
            // 菱（方）形搜索,进一步求精
            /* square refine */
            bcost <<= 4;
            COST_MV_X4_DIR(  0,-1,  0,1, -1,0, 1,0, costs );
            COPY1_IF_LT( bcost, (costs[0]<<4)+1 );
            COPY1_IF_LT( bcost, (costs[1]<<4)+2 );
            COPY1_IF_LT( bcost, (costs[2]<<4)+3 );
            COPY1_IF_LT( bcost, (costs[3]<<4)+4 );
            COST_MV_X4_DIR( -1,-1, -1,1, 1,-1, 1,1, costs );
            COPY1_IF_LT( bcost, (costs[0]<<4)+5 );
            COPY1_IF_LT( bcost, (costs[1]<<4)+6 );
            COPY1_IF_LT( bcost, (costs[2]<<4)+7 );
            COPY1_IF_LT( bcost, (costs[3]<<4)+8 );
            bmx += square1[bcost&15][0];
            bmy += square1[bcost&15][1];
            bcost >>= 4;
            break;
        }
        // （3）非对称十字型多层次六边形格点搜索算法（UMH）
        // 该方法用到了下图所示的多个搜索模板,相对比较复杂,目前还没有仔细研究.记录一下步骤：
        // 步骤0：进行一次小菱形搜索,根据匹配误差值和两个门限值（对于一种尺寸的宏块来说是固定大小的threshold1和threshold2）之间的关系作相应的处理,可能用到中菱形模板或者正八边形模板,也有可能直接跳到步骤1.
        // 步骤1：使用非对称十字模板搜索.“非对称”的原因是一般水平方向运动要比垂直方向运动剧烈,所以将水平方向搜索范围定为W,垂直方向搜索范围定为W/2.
        // 步骤2：使用5x5逐步搜索模板搜索.
        // 步骤3：使用大六边形模板搜索.
        // 步骤4：使用六边形搜索算法找到最优匹配点.
        case X264_ME_UMH:
        {
            /* Uneven-cross Multi-Hexagon-grid Search
             * as in JM, except with different early termination */

            static const uint8_t pixel_size_shift[7] = { 0, 1, 1, 2, 3, 3, 4 };

            int ucost1, ucost2;
            int cross_start = 1;

            /* refine predictors */
            ucost1 = bcost;
           
            // 以pmv为中心,取构成菱形的四个点进行预测求SAD,
            // 如果有更小的,将替换bcost和bmx, bmy
            DIA1_ITER( pmx, pmy );
            if( pmx | pmy ) // 以0向量为中心,取构成菱形的四个点预测
                DIA1_ITER( 0, 0 );

            // 为什么？？？
            if( i_pixel == PIXEL_4x4 )
                goto me_hex2;

            ucost2 = bcost;
            // 如果bmx, bmy不是0向量, 且不等于pmv向量
            // 搜索以bmx, bmy为中心构成菱形的四个点进行预测,
            // 如果有更小的代价,则替换bcost和bmx, bmy
            if( (bmx | bmy) && ((bmx-pmx) | (bmy-pmy)) )
                DIA1_ITER( bmx, bmy );
            // 如果上面的预测,没有预测到更佳的预测运动向量
            // 则将cross_start赋值为3
            if( bcost == ucost2 )
                cross_start = 3;
               
            // 保存当前的bmx, bmy
            omx = bmx; omy = bmy;

            /* early termination */
#define SAD_THRESH(v) ( bcost < ( v >> pixel_size_shift[i_pixel] ) )
            if( bcost == ucost2 && SAD_THRESH(2000) )
            {
                // 如果bcost == ucost2, 并且bcost小于某个阈值
                // 以omx, omy为中心,对其周围的四个点进行预测
                // 如果有更小的代价, 则替换bcost和bmx, bmy
                COST_MV_X4( 0,-2, -1,-1, 1,-1, -2,0 );
                // 以omx, omy为中心, 对其周围的另外四个点进行预测
                // 如果有更小的代价,则替换bcost和bmx, bmy
                COST_MV_X4( 2, 0, -1, 1, 1, 1,  0,2 );
                // 如果bcost == ucost1,说明前面的预测都没有发现
                // 代价更小的预测点,因此提前终止预测
                if( bcost == ucost1 && SAD_THRESH(500) )
                    break;
                // 如果bcost == ucost2,则继续预测
                if( bcost == ucost2 )
                {
                    // 设定预测范围
                    int range = (i_me_range>>1) | 1;
                    // x,y方向交叉预测omx, omy附近的点, 见CROSS
                    // 如果有更小的代价,则替换bcost和bmx, bmy
                    CROSS( 3, range, range );
                    // 预测omx, omy为中心的下述四个点
                    // 如果有更小的代价,则替换bcost和bmx, bmy
                    COST_MV_X4( -1,-2, 1,-2, -2,-1, 2,-1 );
                    // 预测omx, omy为中心的下述另外四个点
                    // 如果有更小的代价,则替换bcost和bmx, bmy
                    COST_MV_X4( -2, 1, 2, 1, -1, 2, 1, 2 );
                    // 如果依然没有搜索到更佳的预测运动向量
                    // 则终止搜索
                    if( bcost == ucost2 )
                        break;
                    // 如果有更佳的预测运动向量被搜索到,
                    // 则设定新的cross_start
                    cross_start = range + 2;
                }
            }

            // 自适应预测搜索
            /* adaptive search range */
            if( i_mvc )
            {
                /* range multipliers based on casual inspection of some statistics of
                 * average distance between current predictor and final mv found by ESA.
                 * these have not been tuned much by actual encoding. */
                static const uint8_t range_mul[4][4] =
                {
                    { 3, 3, 4, 4 },
                    { 3, 4, 4, 4 },
                    { 4, 4, 4, 5 },
                    { 4, 4, 5, 6 },
                };
                int mvd;
                int sad_ctx, mvd_ctx;
                int denom = 1;

                if( i_mvc == 1 )
                {
                    if( i_pixel == PIXEL_16x16 )
                        /* mvc is probably the same as mvp, so the difference isn't meaningful.
                         * but prediction usually isn't too bad, so just use medium range */
                        mvd = 25;
                    else
                        mvd = abs( m->mvp[0] - mvc[0][0] )
                            + abs( m->mvp[1] - mvc[0][1] );
                }
                else
                {
                    /* calculate the degree of agreement between predictors. */
                    /* in 16x16, mvc includes all the neighbors used to make mvp,
                     * so don't count mvp separately. */
                    // 获得mvc的相邻格数
                    denom = i_mvc - 1;
                    // 初始化向量差的绝对值和
                    mvd = 0;
                    if( i_pixel != PIXEL_16x16 )
                    {
                        // 如果i_pixel不等于 0
                        // 计算mvp,与mvc[0]两个向量的x,y方向
                        // 的差的绝对值和
                        mvd = abs( m->mvp[0] - mvc[0][0] )
                            + abs( m->mvp[1] - mvc[0][1] );
                        // 增加格数
                        denom++;
                    }
                    // 迭代计算mvc数组相邻两个mvc[i], mvc[i+1]之间的
                    // 的向量差的绝对值之和
                    mvd += x264_predictor_difference( mvc, i_mvc );
                }
                // 根据bcost与阈值之间的关系, 设定sad_ctx
                sad_ctx = SAD_THRESH(1000) ? 0
                        : SAD_THRESH(2000) ? 1
                        : SAD_THRESH(4000) ? 2 : 3;
                // 根据上面计算的mvd及denom, 设定mvc_ctx
                mvd_ctx = mvd < 10*denom ? 0
                        : mvd < 20*denom ? 1
                        : mvd < 40*denom ? 2 : 3;
                // 获取相应的搜索范围放大器, 更新搜索范围
                i_me_range = i_me_range * range_mul[mvd_ctx][sad_ctx] >> 2;
            }

            /* FIXME if the above DIA2/OCT2/CROSS found a new mv, it has not updated omx/omy.
             * we are still centered on the same place as the DIA2. is this desirable? */
            // 在x, y方向交叉搜索, x方向的搜索范围i_me_range,
            // y方向搜索范围i_me_range / 2
            // 如果发现更佳的运动向量, 则替换bcost和bmx, bmy
            CROSS( cross_start, i_me_range, i_me_range>>1 );
            // 预测以omx, omy为中心的下述四个点
            // 如果发现更佳的运动向量,则替换bcost和bmx, bmy
            COST_MV_X4( -2,-2, -2,2, 2,-2, 2,2 );

            /* hexagon grid */
            // 保存新的候选的最佳预测运动向量
            omx = bmx; omy = bmy;
           
            // 让p_cost_omx, p_cost_omvy
            // 指向p_cost_mvx, p_cost_mvy的某个偏移
        // a->p_cost_mv = h->cost_mv[a->i_qp] 见 x264_mb_analyse_load_costs
        // h->cost_mv 初始化, 见 x264_analyse_init_costs
        // m->p_cost_mv = a->p_cost_mv 见宏定义 LOAD_FUNC
   
            const uint16_t *p_cost_omvx = p_cost_mvx + omx*4;
            const uint16_t *p_cost_omvy = p_cost_mvy + omy*4;
            int i = 1;
            do
            {
                static const int8_t hex4[16][2] = {
                    { 0,-4}, { 0, 4}, {-2,-3}, { 2,-3},
                    {-4,-2}, { 4,-2}, {-4,-1}, { 4,-1},
                    {-4, 0}, { 4, 0}, {-4, 1}, { 4, 1},
                    {-4, 2}, { 4, 2}, {-2, 3}, { 2, 3},
                };
                // 如果 4*i 大于这四个值的最小值
                if( 4*i > X264_MIN4( mv_x_max-omx, omx-mv_x_min,
                                     mv_y_max-omy, omy-mv_y_min ) )
                {
                    // 迭代搜索 16 次
                    for( int j = 0; j < 16; j++ )
                    {
                        // 将搜索点偏移某个向量
                        int mx = omx + hex4[j][0]*i;
                        int my = omy + hex4[j][1]*i;
                        // 新的预测运动向量是否越界
                        // 如果没有越界,则预测其代价
                        // 如果代价更小,则替换bcost和bmx, bmy
                        if( CHECK_MVRANGE(mx, my) )
                            COST_MV( mx, my );
                    }
                }
                else
                {
                    int dir = 0;
                    // 根据omx, omy, i 调整预测中心
                    pixel *pix_base = p_fref_w + omx + (omy-4*i)*stride;
                    int dy = i*stride;
// h->pixf.fpelcmp_x4[0] = x264_pixel_sad_x4_16x16
// 这个宏定义有点特别, 这种语法pix_base x0*i+(y0-2*k+4)*dy
// 能表示pix_base的偏移 x0*i+(y0-2*k+4)*dy
// 我自己写类似的语法,根本无法编译
// 不知道 x264 如何做到
#define SADS(k,x0,y0,x1,y1,x2,y2,x3,y3)\
                    h->pixf.fpelcmp_x4[i_pixel]( p_fenc,\
                            pix_base x0*i+(y0-2*k+4)*dy,\
                            pix_base x1*i+(y1-2*k+4)*dy,\
                            pix_base x2*i+(y2-2*k+4)*dy,\
                            pix_base x3*i+(y3-2*k+4)*dy,\
                            stride, costs+4*k );\
                    pix_base += 2*dy;
#define ADD_MVCOST(k,x,y) costs[k] += p_cost_omvx[x*4*i] + p_cost_omvy[y*4*i]
#define MIN_MV(k,x,y)     COPY2_IF_LT( bcost, costs[k], dir, x*16+(y&15) )
                    // 根据新的中心点, 预测其四个偏移点
                    // pix_base x0*i+(y0-2*k+4)*dy  等
                    // 其中 dy = i * stride; // stride在我的调试中=1344
                    // 并分别将预测代价保存在costs数组
                    SADS( 0, +0,-4, +0,+4, -2,-3, +2,-3 );
                    SADS( 1, -4,-2, +4,-2, -4,-1, +4,-1 );
                    SADS( 2, -4,+0, +4,+0, -4,+1, +4,+1 );
                    SADS( 3, -4,+2, +4,+2, -2,+3, +2,+3 );
                    // 将每个点预测代价加上该点的预设的相关代价
                    ADD_MVCOST(  0, 0,-4 );
                    ADD_MVCOST(  1, 0, 4 );
                    ADD_MVCOST(  2,-2,-3 );
                    ADD_MVCOST(  3, 2,-3 );
                    ADD_MVCOST(  4,-4,-2 );
                    ADD_MVCOST(  5, 4,-2 );
                    ADD_MVCOST(  6,-4,-1 );
                    ADD_MVCOST(  7, 4,-1 );
                    ADD_MVCOST(  8,-4, 0 );
                    ADD_MVCOST(  9, 4, 0 );
                    ADD_MVCOST( 10,-4, 1 );
                    ADD_MVCOST( 11, 4, 1 );
                    ADD_MVCOST( 12,-4, 2 );
                    ADD_MVCOST( 13, 4, 2 );
                    ADD_MVCOST( 14,-2, 3 );
                    ADD_MVCOST( 15, 2, 3 );
                    // #define MIN_MV(k,x,y)    
                    // COPY2_IF_LT( bcost, costs[k], dir, x*16+(y&15) )
                    // 判断该点的预测代价是否更优
                    // 如果更佳,则替换bcost, 并将x, y进行复合
                    // 赋值给dir
                    MIN_MV(  0, 0,-4 );
                    MIN_MV(  1, 0, 4 );
                    MIN_MV(  2,-2,-3 );
                    MIN_MV(  3, 2,-3 );
                    MIN_MV(  4,-4,-2 );
                    MIN_MV(  5, 4,-2 );
                    MIN_MV(  6,-4,-1 );
                    MIN_MV(  7, 4,-1 );
                    MIN_MV(  8,-4, 0 );
                    MIN_MV(  9, 4, 0 );
                    MIN_MV( 10,-4, 1 );
                    MIN_MV( 11, 4, 1 );
                    MIN_MV( 12,-4, 2 );
                    MIN_MV( 13, 4, 2 );
                    MIN_MV( 14,-2, 3 );
                    MIN_MV( 15, 2, 3 );
#undef SADS
#undef ADD_MVCOST
#undef MIN_MV
                    // 如果dir不为0, 则说明前面的预测
                    // 获得了更佳的预测运动向量
                    if(dir)
                    {
                        // 更新最佳预测运动向量
                        // 为什么乘以i,暂时不是很清楚
                        bmx = omx + i*(dir>>4);
                        bmy = omy + i*((dir<<28)>>28);
                    }
                }
            } while( ++i <= i_me_range>>2 ); // i 是否越界
           
            // 如果最佳预测运动向量没有越界,进行hex2预测
            // 为什么没有越界,继续进行hex预测
            // 而越界后终止
            if( bmy <= mv_y_max && bmy >= mv_y_min && bmx <= mv_x_max && bmx >= mv_x_min )
                goto me_hex2;
               
            // 终止预测
            break;
        }
        // 该方法是一种全搜索算法,它对搜索区域内的点进行光栅式搜索,逐一计算并比较.
        case X264_ME_ESA:
        case X264_ME_TESA:
        {
            // 限制 min_x, min_y, max_x, max_y 的范围
            const int min_x = X264_MAX( bmx - i_me_range, mv_x_min );
            const int min_y = X264_MAX( bmy - i_me_range, mv_y_min );
            const int max_x = X264_MIN( bmx + i_me_range, mv_x_max );
            const int max_y = X264_MIN( bmy + i_me_range, mv_y_max );
           
            // 获取 x 方向宽度,
            // 除以4, 是否min_x, max_x 是 1/4像素坐标
            // 转换为整像素坐标 ？？？
            /* SEA is fastest in multiples of 4 */
            const int width = (max_x - min_x + 3) & ~3;
#if 0
            /* plain old exhaustive search */
            for( int my = min_y; my <= max_y; my++ )
                for( int mx = min_x; mx < min_x + width; mx++ )
                    COST_MV( mx, my );
#else
            /* successive elimination by comparing DC before a full SAD,
             * because sum(abs(diff)) >= abs(diff(sum)). */
            uint16_t *sums_base = m->integral;
            // 声明一个整数数组 int enc_dc[4]
            ALIGNED_ARRAY_16( int, enc_dc,[4] );
            // PIXEL_8X8 = 3, PIXEL_4X4 = 6
            // 因此 sad_size 要么=3, 要么=6
            // sad_size在这是x264_pixel_size的索引号
            // enum
      // {
      //     PIXEL_16x16 = 0,
      //     PIXEL_16x8  = 1,
      //     PIXEL_8x16  = 2,
      //     PIXEL_8x8   = 3,
      //     PIXEL_8x4   = 4,
      //     PIXEL_4x8   = 5,
      //     PIXEL_4x4   = 6,
       
      //     /* Subsampled chroma only */
      //     PIXEL_4x16  = 7,  /* 4:2:2 */
      //     PIXEL_4x2   = 8,
      //     PIXEL_2x8   = 9,  /* 4:2:2 */
      //     PIXEL_2x4   = 10,
      //     PIXEL_2x2   = 11,
      // };
       
      // static const struct { uint8_t w, h; } x264_pixel_size[12] =
      // {
      //     { 16, 16 }, { 16, 8 }, { 8, 16 }, { 8, 8 }, { 8, 4 }, { 4, 8 }, { 4, 4 },
      //     {  4, 16 }, {  4, 2 }, { 2,  8 }, { 2, 4 }, { 2, 2 },
      // };
            // x264_pixel_size表示的是像素的wxh
           
            int sad_size = i_pixel <= PIXEL_8x8 ? PIXEL_8x8 : PIXEL_4x4;
            int delta = x264_pixel_size[sad_size].w;
            int16_t *xs = h->scratch_buffer;
            int xn;
           
            // 在 x264_analyse_init_costs 函数中
       //  if( h->param.analyse.i_me_method >= X264_ME_ESA && !h->cost_mv_fpel[qp][0] )
       //  {
       //      for( int j = 0; j < 4; j++ )
       //      {
       //          CHECKED_MALLOC( h->cost_mv_fpel[qp][j], (4*2048 + 1) * sizeof(uint16_t) );
       //          h->cost_mv_fpel[qp][j] += 2*2048;
       //          for( int i = -2*2048; i < 2*2048; i++ )
       //              h->cost_mv_fpel[qp][j][i] = h->cost_mv[qp][i*4+j];
       //      }
       //  }
       // 由上面的代码可知, h->cost_mv_fpel数组
       // 在x264_analyse_init_costs被初始化
       // 因此 cost_fpel_mvx指向某个指针
       // 从字面上看, 这块内存保存着整像素预设的预测向量？？？
            uint16_t *cost_fpel_mvx = h->cost_mv_fpel[h->mb.i_qp][-m->mvp[0]&3] + (-m->mvp[0]>>2);

            // 在我的调试中 sad_size = 3, delta = 8
            // h->pixf.sad_x4[3] = x264_pixel_sad_x4_8x8
            // SAD_X( 8x8 )将分别调x264_pixel_sad_8x8三次
            // 如果p_fenc指向的宏块假定为索引0
            // 那么p_fenc表示宏块0
            // p_fenc+delta表示其右宏块1
            // p_fenc + delta*FENC_STRIDE表示其底宏块2
            // p_fenc + delta + delta * FENC_STRIDE表示其底右宏块
            // 也就是p_fenc将与其右, 底, 底右宏块
            // 分别x26_pixel_sad_8x8求SAD
            // 将预测代价保存在enc_dc数组中
            h->pixf.sad_x4[sad_size]( (pixel*)x264_zero, p_fenc, p_fenc+delta,
                p_fenc+delta*FENC_STRIDE, p_fenc+delta+delta*FENC_STRIDE,
                FENC_STRIDE, enc_dc );
               
            // PADV = 32, 在我的调试中 stride = 1344
            // h->fenc->i_lines[0] + PADV*2 表示有多少行（包括padding lines）
            if( delta == 4 )
                sums_base += stride * (h->fenc->i_lines[0] + PADV*2);
            // 在我的调试中, i_pixel = 0(PIXEL_16x16)
            // 因此 delta = 8 * 1344 = 10752
            if( i_pixel == PIXEL_16x16 || i_pixel == PIXEL_8x16 || i_pixel == PIXEL_4x8 )
                delta *= stride;
            if( i_pixel == PIXEL_8x16 || i_pixel == PIXEL_4x8 )
                enc_dc[1] = enc_dc[2];

            if( h->mb.i_me_method == X264_ME_TESA )
            {
                // ADS threshold, then SAD threshold, then keep the best few SADs, then SATD
                mvsad_t *mvsads = (mvsad_t *)(xs + ((width+31)&~31) + 4);
                int nmvsad = 0, limit;
                // 根据 i_me_range 的值 设定 sad_thresh = 10, 11, or 12
                int sad_thresh = i_me_range <= 16 ? 10 : i_me_range <= 24 ? 11 : 12;
               
                // h->pixf.sad[0] = x264_pixel_sad_16x16
                // 计算p_fenc 与 参考Y平面在预测向量bmx, bmy为原点
                // 的sad计算, 得到的SAD赋值给bsad
                int bsad = h->pixf.sad[i_pixel]( p_fenc, FENC_STRIDE, p_fref_w+bmy*stride+bmx, stride )
                         + BITS_MVD( bmx, bmy );
                for( int my = min_y; my <= max_y; my++ )
                {
                    int i;
                    // 初始化y方向预测代价
                    int ycost = p_cost_mvy[my<<2];
                    // 如果bsad小于这个初始化代价值,则进行下一个迭代
                    if( bsad <= ycost )
                        continue;
                       
                    // 得到剩余代价
                    bsad -= ycost;
                    // h->pixf.ads[0] = x264_pixel_ads4
          // static int x264_pixel_ads4( int enc_dc[4], uint16_t *sums, int delta,
          //            uint16_t *cost_mvx, int16_t *mvs, int width, int thresh )
          // {
          //    int nmv = 0;
          //    for( int i = 0; i < width; i++, sums++ )
          //    {
          //        int ads = abs( enc_dc[0] - sums[0] )
          //                + abs( enc_dc[1] - sums[8] )
          //                + abs( enc_dc[2] - sums[delta] )
          //                + abs( enc_dc[3] - sums[delta+8] )
          //                + cost_mvx[i];
          //        if( ads < thresh )
          //            mvs[nmv++] = i;
          //    }
          //    return nmv;
          // }
                   
                    xn = h->pixf.ads[i_pixel]( enc_dc, sums_base + min_x + my * stride, delta,
                                               cost_fpel_mvx+min_x, xs, width, bsad * 17 >> 4 );
                   
                    for( i = 0; i < xn-2; i += 3 )
                    {
                        // 指向min_X, my为原点的像素区
                        pixel *ref = p_fref_w+min_x+my*stride;
                        // 声明 int sads[4]
                        ALIGNED_ARRAY_16( int, sads,[4] ); /* padded to [4] for asm */
                       
                        // h->pixf.sad_x3[0] = x264_pixel_sad_x3_16x16
                        // 求p_fenc分别于ref + xs[i], ref + xs[i+1], ref + xs[i+2]
                        // 三个像素点为原点的预测代价,并保存在sads数组
                        h->pixf.sad_x3[i_pixel]( p_fenc, ref+xs[i], ref+xs[i+1], ref+xs[i+2], stride, sads );
                        for( int j = 0; j < 3; j++ )
                        {
                            // 迭代求sad
                            int sad = sads[j] + cost_fpel_mvx[xs[i+j]];
                            // sad 满足某个条件
                            if( sad < bsad*sad_thresh>>3 )
                            {
                                // 如果sad < bsad, => bsad = sad
                                COPY1_IF_LT( bsad, sad );
                                // 保存补偿后的sad
                                mvsads[nmvsad].sad = sad + ycost;
                                // 保存候选的预测运动向量
                                mvsads[nmvsad].mv[0] = min_x+xs[i+j];
                                mvsads[nmvsad].mv[1] = my;
                                nmvsad++;
                            }
                        }
                    }
                    for( ; i < xn; i++ )
                    {
                        // 获取新的mx, 作为x方向的原点
                        int mx = min_x+xs[i];
                        // 在p_fenc和以mx, my为原点参考Y平面之间
                        // 求sad
                        int sad = h->pixf.sad[i_pixel]( p_fenc, FENC_STRIDE, p_fref_w+mx+my*stride, stride )
                                + cost_fpel_mvx[xs[i]];
                        if( sad < bsad*sad_thresh>>3 )
                        {
                            // 执行类似上面的操作
                            COPY1_IF_LT( bsad, sad );
                            mvsads[nmvsad].sad = sad + ycost;
                            mvsads[nmvsad].mv[0] = mx;
                            mvsads[nmvsad].mv[1] = my;
                            nmvsad++;
                        }
                    }
                    // 补偿 bsad
                    bsad += ycost;
                }

                // 设定新的限制和阈值
                limit = i_me_range >> 1;
                sad_thresh = bsad*sad_thresh>>3;
                while( nmvsad > limit*2 && sad_thresh > bsad )
                {
                    int i;
                    // halve the range if the domain is too large... eh, close enough
                    // 更新阈值
                    sad_thresh = (sad_thresh + bsad) >> 1;
                    // 迭代直到 mvsads[i].sad > sad_thresh
                    while( i < nmvsad && mvsads[i].sad <= sad_thresh )
                        i++;

                    // 从i开始, 拷贝后面的sad, mv到[i]
                    // 这段代码的主要作用是将小于sad_thresh的
                    // sad 和 mv 保存在 mvsads[0], [1], ..., [i - 1]中
                    for( int j = i; j < nmvsad; j++ )
                    {
                        uint32_t sad;
                        if( WORD_SIZE == 8 && sizeof(mvsad_t) == 8 )
                        {
                            uint64_t mvsad = M64( &mvsads[i] ) = M64( &mvsads[j] );
#if WORDS_BIGENDIAN
                            mvsad >>= 32;
#endif
                            sad = mvsad;
                        }
                        else
                        {
                            // 将数组后面的sad, mv拷贝给[i]
                            sad = mvsads[j].sad;
                            CP32( mvsads[i].mv, mvsads[j].mv );
                            mvsads[i].sad = sad;
                        }
                        // 第一遍迭代, i 保持不变,
                        // 因为j = i, 这时 mvsads[i].sad > sad_thresh
                        // 因为sad是uint32_t型,因此右边的取值只可能是0或1
                        i += (sad - (sad_thresh+1)) >> 31;
                    }
                    // 小于阈值的sad数目为i
                    nmvsad = i;
                }
               
                // 这段代码的作用是迭代剔除目前数组中的最大者
                // 知道 nmvsad <= limit
                while( nmvsad > limit )
                {
                    int bi = 0;
                    for( int i = 1; i < nmvsad; i++ )
                        if( mvsads[i].sad > mvsads[bi].sad )
                            bi = i;
                    nmvsad--;
                    if( sizeof( mvsad_t ) == sizeof( uint64_t ) )
                        CP64( &mvsads[bi], &mvsads[nmvsad] );
                    else
                        mvsads[bi] = mvsads[nmvsad];
                }
                // 迭代在p_fenc 和 在以mvsads[i].mv[0], mvsads[i].mv[1]
                // 为原点的参考帧的Y平面求sad
                // 如果预测代价更小, 则替换bcost和bmx, bmy
                for( int i = 0; i < nmvsad; i++ )
                    COST_MV( mvsads[i].mv[0], mvsads[i].mv[1] );
            }
            else 
            {
                // just ADS and SAD
                for( int my = min_y; my <= max_y; my++ )
                {
                    int i;
                    // 初始化ycost
                    int ycost = p_cost_mvy[my*4];
                    if( bcost <= ycost )
                        continue;
                       
                    // 获得剩余代价
                    bcost -= ycost;
                    // h->pixf.ads[0] = x264_pixel_ads4
          // static int x264_pixel_ads4( int enc_dc[4], uint16_t *sums, int delta,
          //            uint16_t *cost_mvx, int16_t *mvs, int width, int thresh )
          // {
          //    int nmv = 0;
          //    for( int i = 0; i < width; i++, sums++ )
          //    {
          //        int ads = abs( enc_dc[0] - sums[0] )
          //                + abs( enc_dc[1] - sums[8] )
          //                + abs( enc_dc[2] - sums[delta] )
          //                + abs( enc_dc[3] - sums[delta+8] )
          //                + cost_mvx[i];
          //        if( ads < thresh )
          //            mvs[nmv++] = i;
          //    }
          //    return nmv;
          // }
                    xn = h->pixf.ads[i_pixel]( enc_dc, sums_base + min_x + my * stride, delta,
                                               cost_fpel_mvx+min_x, xs, width, bcost );
                   

          // #define COST_MV_X3_ABS( m0x, m0y, m1x, m1y, m2x, m2y )\
          // {\
          //      // h->pixf.fpelcmp_x3 = x264_pixel_sad_x3_16x16
          //     h->pixf.fpelcmp_x3[i_pixel]( p_fenc,\
          //         p_fref_w + (m0x) + (m0y)*stride,\
          //         p_fref_w + (m1x) + (m1y)*stride,\
          //         p_fref_w + (m2x) + (m2y)*stride,\
          //         stride, costs );\
          //     costs[0] += p_cost_mvx[(m0x)<<2]; /* no cost_mvy */\
          //     costs[1] += p_cost_mvx[(m1x)<<2];\
          //     costs[2] += p_cost_mvx[(m2x)<<2];\
          //     COPY3_IF_LT( bcost, costs[0], bmx, m0x, bmy, m0y );\
          //     COPY3_IF_LT( bcost, costs[1], bmx, m1x, bmy, m1y );\
          //     COPY3_IF_LT( bcost, costs[2], bmx, m2x, bmy, m2y );\
          // }
                    // 迭代预测三个点, 如果有更佳的候选者, 则替换bcost, bmx, bmy
                    for( i = 0; i < xn-2; i += 3 )
                        COST_MV_X3_ABS( min_x+xs[i],my, min_x+xs[i+1],my, min_x+xs[i+2],my );
                   
                    // 补偿bcost
                    bcost += ycost;
                    // 迭代计算在p_fenc和以min_x + xs[i], my为原点的参考帧
                    // Y平面之间的SAD
                    // 如果有更佳的, 替换bcost和bmx, bmy
                    for( ; i < xn; i++ )
                        COST_MV( min_x+xs[i], my );
                }
            }
#endif
        }
        break;
    }

    /* -> qpel mv */
   
    // 打包候选的预测运动向量
    uint32_t bmv = pack16to32_mask(bmx,bmy);
    // 打包1/4像素坐标
    uint32_t bmv_spel = SPELx2(bmv);
    if( h->mb.i_subpel_refine < 3 )
    {
        m->cost_mv = p_cost_mvx[bmx*4] + p_cost_mvy[bmy*4];
        // 设定最后的cost
        m->cost = bcost;
        /* compute the real cost */
        if( bmv == pmv ) m->cost += m->cost_mv;
        // 设定最佳的预测运动向量
        M32( m->mv ) = bmv_spel;
    }
    else
    {
        // 根据bpred_cost(from mvp) 与 bcost的大小
        // 选择最佳预测运动向量 bpred_mv 或 bmv_spel
        // 同时也选择最小代价
        M32(m->mv) = bpred_cost < bcost ? bpred_mv : bmv_spel;
        m->cost = X264_MIN( bpred_cost, bcost );
    }

    /* subpel refine */
    if( h->mb.i_subpel_refine >= 2 )
    {
        // 子像素预测,进一步精化提炼
        int hpel = subpel_iterations[h->mb.i_subpel_refine][2];
        int qpel = subpel_iterations[h->mb.i_subpel_refine][3];
        // 参见 refine_subpel 分析
        refine_subpel( h, m, hpel, qpel, p_halfpel_thresh, 0 );
    }
}
#undef COST_MV

void x264_me_refine_qpel( x264_t *h, x264_me_t *m )
{
    int hpel = subpel_iterations[h->mb.i_subpel_refine][0];
    int qpel = subpel_iterations[h->mb.i_subpel_refine][1];

    if( m->i_pixel <= PIXEL_8x8 )
        m->cost -= m->i_ref_cost;

    refine_subpel( h, m, hpel, qpel, NULL, 1 );
}

void x264_me_refine_qpel_refdupe( x264_t *h, x264_me_t *m, int *p_halfpel_thresh )
{
    refine_subpel( h, m, 0, X264_MIN( 2, subpel_iterations[h->mb.i_subpel_refine][3] ), p_halfpel_thresh, 0 );
}

#define COST_MV_SAD( mx, my ) \
{ \
    intptr_t stride = 16; \
    pixel *src = h->mc.get_ref( pix, &stride, m->p_fref, m->i_stride[0], mx, my, bw, bh, &m->weight[0] ); \
    int cost = h->pixf.fpelcmp[i_pixel]( m->p_fenc[0], FENC_STRIDE, src, stride ) \
             + p_cost_mvx[ mx ] + p_cost_mvy[ my ]; \
    COPY3_IF_LT( bcost, cost, bmx, mx, bmy, my ); \
}

#define COST_MV_SATD( mx, my, dir ) \
if( b_refine_qpel || (dir^1) != odir ) \
{ \
    intptr_t stride = 16; \
    pixel *src = h->mc.get_ref( pix, &stride, &m->p_fref[0], m->i_stride[0], mx, my, bw, bh, &m->weight[0] ); \
    int cost = h->pixf.mbcmp_unaligned[i_pixel]( m->p_fenc[0], FENC_STRIDE, src, stride ) \
             + p_cost_mvx[ mx ] + p_cost_mvy[ my ]; \
    if( b_chroma_me && cost < bcost ) \
    { \
        if( CHROMA444 ) \
        { \
            stride = 16; \
            src = h->mc.get_ref( pix, &stride, &m->p_fref[4], m->i_stride[1], mx, my, bw, bh, &m->weight[1] ); \
            cost += h->pixf.mbcmp_unaligned[i_pixel]( m->p_fenc[1], FENC_STRIDE, src, stride ); \
            if( cost < bcost ) \
            { \
                stride = 16; \
                src = h->mc.get_ref( pix, &stride, &m->p_fref[8], m->i_stride[2], mx, my, bw, bh, &m->weight[2] ); \
                cost += h->pixf.mbcmp_unaligned[i_pixel]( m->p_fenc[2], FENC_STRIDE, src, stride ); \
            } \
        } \
        else \
        { \
            h->mc.mc_chroma( pix, pix+8, 16, m->p_fref[4], m->i_stride[1], \
                             mx, 2*(my+mvy_offset)>>chroma_v_shift, bw>>1, bh>>chroma_v_shift ); \
            if( m->weight[1].weightfn ) \
                m->weight[1].weightfn[bw>>3]( pix, 16, pix, 16, &m->weight[1], bh>>chroma_v_shift ); \
            cost += h->pixf.mbcmp[chromapix]( m->p_fenc[1], FENC_STRIDE, pix, 16 ); \
            if( cost < bcost ) \
            { \
                if( m->weight[2].weightfn ) \
                    m->weight[2].weightfn[bw>>3]( pix+8, 16, pix+8, 16, &m->weight[2], bh>>chroma_v_shift ); \
                cost += h->pixf.mbcmp[chromapix]( m->p_fenc[2], FENC_STRIDE, pix+8, 16 ); \
            } \
        } \
    } \
    COPY4_IF_LT( bcost, cost, bmx, mx, bmy, my, bdir, dir ); \
}

static void refine_subpel( x264_t *h, x264_me_t *m, int hpel_iters, int qpel_iters, int *p_halfpel_thresh, int b_refine_qpel )
{
    const int bw = x264_pixel_size[m->i_pixel].w;
    const int bh = x264_pixel_size[m->i_pixel].h;
    const uint16_t *p_cost_mvx = m->p_cost_mv - m->mvp[0];
    const uint16_t *p_cost_mvy = m->p_cost_mv - m->mvp[1];
    const int i_pixel = m->i_pixel;
    const int b_chroma_me = h->mb.b_chroma_me && (i_pixel <= PIXEL_8x8 || CHROMA444);
    int chromapix = h->luma2chroma_pixel[i_pixel];
    int chroma_v_shift = CHROMA_V_SHIFT;
    int mvy_offset = chroma_v_shift & MB_INTERLACED & m->i_ref ? (h->mb.i_mb_y & 1)*4 - 2 : 0;

    ALIGNED_ARRAY_32( pixel, pix,[64*18] ); // really 17x17x2, but round up for alignment
    ALIGNED_ARRAY_16( int, costs,[4] );

    int bmx = m->mv[0];
    int bmy = m->mv[1];
    int bcost = m->cost;
    int odir = -1, bdir;

    /* halfpel diamond search */
    if( hpel_iters )
    {
        /* try the subpel component of the predicted mv */
        if( h->mb.i_subpel_refine < 3 )
        {
            int mx = x264_clip3( m->mvp[0], h->mb.mv_min_spel[0]+2, h->mb.mv_max_spel[0]-2 );
            int my = x264_clip3( m->mvp[1], h->mb.mv_min_spel[1]+2, h->mb.mv_max_spel[1]-2 );
            if( (mx-bmx)|(my-bmy) )
                COST_MV_SAD( mx, my );
        }

        bcost <<= 6;
        for( int i = hpel_iters; i > 0; i-- )
        {
            int omx = bmx, omy = bmy;
            intptr_t stride = 64; // candidates are either all hpel or all qpel, so one stride is enough
            pixel *src0, *src1, *src2, *src3;
            src0 = h->mc.get_ref( pix,    &stride, m->p_fref, m->i_stride[0], omx, omy-2, bw, bh+1, &m->weight[0] );
            src2 = h->mc.get_ref( pix+32, &stride, m->p_fref, m->i_stride[0], omx-2, omy, bw+4, bh, &m->weight[0] );
            src1 = src0 + stride;
            src3 = src2 + 1;
            h->pixf.fpelcmp_x4[i_pixel]( m->p_fenc[0], src0, src1, src2, src3, stride, costs );
            costs[0] += p_cost_mvx[omx  ] + p_cost_mvy[omy-2];
            costs[1] += p_cost_mvx[omx  ] + p_cost_mvy[omy+2];
            costs[2] += p_cost_mvx[omx-2] + p_cost_mvy[omy  ];
            costs[3] += p_cost_mvx[omx+2] + p_cost_mvy[omy  ];
            COPY1_IF_LT( bcost, (costs[0]<<6)+2 );
            COPY1_IF_LT( bcost, (costs[1]<<6)+6 );
            COPY1_IF_LT( bcost, (costs[2]<<6)+16 );
            COPY1_IF_LT( bcost, (costs[3]<<6)+48 );
            if( !(bcost&63) )
                break;
            bmx -= (int32_t)((uint32_t)bcost<<26)>>29;
            bmy -= (int32_t)((uint32_t)bcost<<29)>>29;
            bcost &= ~63;
        }
        bcost >>= 6;
    }

    if( !b_refine_qpel && (h->pixf.mbcmp_unaligned[0] != h->pixf.fpelcmp[0] || b_chroma_me) )
    {
        bcost = COST_MAX;
        COST_MV_SATD( bmx, bmy, -1 );
    }

    /* early termination when examining multiple reference frames */
    if( p_halfpel_thresh )
    {
        if( (bcost*7)>>3 > *p_halfpel_thresh )
        {
            m->cost = bcost;
            m->mv[0] = bmx;
            m->mv[1] = bmy;
            // don't need cost_mv
            return;
        }
        else if( bcost < *p_halfpel_thresh )
            *p_halfpel_thresh = bcost;
    }

    /* quarterpel diamond search */
    if( h->mb.i_subpel_refine != 1 )
    {
        bdir = -1;
        for( int i = qpel_iters; i > 0; i-- )
        {
            if( bmy <= h->mb.mv_min_spel[1] || bmy >= h->mb.mv_max_spel[1] || bmx <= h->mb.mv_min_spel[0] || bmx >= h->mb.mv_max_spel[0] )
                break;
            odir = bdir;
            int omx = bmx, omy = bmy;
            COST_MV_SATD( omx, omy - 1, 0 );
            COST_MV_SATD( omx, omy + 1, 1 );
            COST_MV_SATD( omx - 1, omy, 2 );
            COST_MV_SATD( omx + 1, omy, 3 );
            if( (bmx == omx) & (bmy == omy) )
                break;
        }
    }
    /* Special simplified case for subme=1 */
    else if( bmy > h->mb.mv_min_spel[1] && bmy < h->mb.mv_max_spel[1] && bmx > h->mb.mv_min_spel[0] && bmx < h->mb.mv_max_spel[0] )
    {
        int omx = bmx, omy = bmy;
        /* We have to use mc_luma because all strides must be the same to use fpelcmp_x4 */
        h->mc.mc_luma( pix   , 64, m->p_fref, m->i_stride[0], omx, omy-1, bw, bh, &m->weight[0] );
        h->mc.mc_luma( pix+16, 64, m->p_fref, m->i_stride[0], omx, omy+1, bw, bh, &m->weight[0] );
        h->mc.mc_luma( pix+32, 64, m->p_fref, m->i_stride[0], omx-1, omy, bw, bh, &m->weight[0] );
        h->mc.mc_luma( pix+48, 64, m->p_fref, m->i_stride[0], omx+1, omy, bw, bh, &m->weight[0] );
        h->pixf.fpelcmp_x4[i_pixel]( m->p_fenc[0], pix, pix+16, pix+32, pix+48, 64, costs );
        costs[0] += p_cost_mvx[omx  ] + p_cost_mvy[omy-1];
        costs[1] += p_cost_mvx[omx  ] + p_cost_mvy[omy+1];
        costs[2] += p_cost_mvx[omx-1] + p_cost_mvy[omy  ];
        costs[3] += p_cost_mvx[omx+1] + p_cost_mvy[omy  ];
        bcost <<= 4;
        COPY1_IF_LT( bcost, (costs[0]<<4)+1 );
        COPY1_IF_LT( bcost, (costs[1]<<4)+3 );
        COPY1_IF_LT( bcost, (costs[2]<<4)+4 );
        COPY1_IF_LT( bcost, (costs[3]<<4)+12 );
        bmx -= (int32_t)((uint32_t)bcost<<28)>>30;
        bmy -= (int32_t)((uint32_t)bcost<<30)>>30;
        bcost >>= 4;
    }

    m->cost = bcost;
    m->mv[0] = bmx;
    m->mv[1] = bmy;
    m->cost_mv = p_cost_mvx[bmx] + p_cost_mvy[bmy];
}

#define BIME_CACHE( dx, dy, list )\
{\
    x264_me_t *m = m##list;\
    int i = 4 + 3*dx + dy;\
    int mvx = bm##list##x+dx;\
    int mvy = bm##list##y+dy;\
    stride[0][list][i] = bw;\
    src[0][list][i] = h->mc.get_ref( pixy_buf[list][i], &stride[0][list][i], &m->p_fref[0],\
                                     m->i_stride[0], mvx, mvy, bw, bh, x264_weight_none );\
    if( rd )\
    {\
        if( CHROMA444 )\
        {\
            stride[1][list][i] = bw;\
            src[1][list][i] = h->mc.get_ref( pixu_buf[list][i], &stride[1][list][i], &m->p_fref[4],\
                                             m->i_stride[1], mvx, mvy, bw, bh, x264_weight_none );\
            stride[2][list][i] = bw;\
            src[2][list][i] = h->mc.get_ref( pixv_buf[list][i], &stride[2][list][i], &m->p_fref[8],\
                                             m->i_stride[2], mvx, mvy, bw, bh, x264_weight_none );\
        }\
        else if( CHROMA_FORMAT )\
            h->mc.mc_chroma( pixu_buf[list][i], pixv_buf[list][i], 8, m->p_fref[4], m->i_stride[1],\
                             mvx, 2*(mvy+mv##list##y_offset)>>chroma_v_shift, bw>>1, bh>>chroma_v_shift );\
    }\
}

#define SATD_THRESH(cost) (cost+(cost>>4))

/* Don't unroll the BIME_CACHE loop. I couldn't find any way to force this
 * other than making its iteration count not a compile-time constant. */
#define x264_iter_kludge x264_template(iter_kludge)
int x264_iter_kludge = 0;

static ALWAYS_INLINE void me_refine_bidir( x264_t *h, x264_me_t *m0, x264_me_t *m1, int i_weight, int i8, int i_lambda2, int rd )
{
    int x = i8&1;
    int y = i8>>1;
    int s8 = X264_SCAN8_0 + 2*x + 16*y;
    int16_t *cache0_mv = h->mb.cache.mv[0][s8];
    int16_t *cache1_mv = h->mb.cache.mv[1][s8];
    const int i_pixel = m0->i_pixel;
    const int bw = x264_pixel_size[i_pixel].w;
    const int bh = x264_pixel_size[i_pixel].h;
    ALIGNED_ARRAY_32( pixel, pixy_buf,[2],[9][16*16] );
    ALIGNED_ARRAY_32( pixel, pixu_buf,[2],[9][16*16] );
    ALIGNED_ARRAY_32( pixel, pixv_buf,[2],[9][16*16] );
    pixel *src[3][2][9];
    int chromapix = h->luma2chroma_pixel[i_pixel];
    int chroma_v_shift = CHROMA_V_SHIFT;
    int chroma_x = (8 >> CHROMA_H_SHIFT) * x;
    int chroma_y = (8 >> chroma_v_shift) * y;
    pixel *pix  = &h->mb.pic.p_fdec[0][8*x + 8*y*FDEC_STRIDE];
    pixel *pixu = CHROMA_FORMAT ? &h->mb.pic.p_fdec[1][chroma_x + chroma_y*FDEC_STRIDE] : NULL;
    pixel *pixv = CHROMA_FORMAT ? &h->mb.pic.p_fdec[2][chroma_x + chroma_y*FDEC_STRIDE] : NULL;
    int ref0 = h->mb.cache.ref[0][s8];
    int ref1 = h->mb.cache.ref[1][s8];
    const int mv0y_offset = chroma_v_shift & MB_INTERLACED & ref0 ? (h->mb.i_mb_y & 1)*4 - 2 : 0;
    const int mv1y_offset = chroma_v_shift & MB_INTERLACED & ref1 ? (h->mb.i_mb_y & 1)*4 - 2 : 0;
    intptr_t stride[3][2][9];
    int bm0x = m0->mv[0];
    int bm0y = m0->mv[1];
    int bm1x = m1->mv[0];
    int bm1y = m1->mv[1];
    int bcost = COST_MAX;
    int mc_list0 = 1, mc_list1 = 1;
    uint64_t bcostrd = COST_MAX64;
    uint16_t amvd;
    /* each byte of visited represents 8 possible m1y positions, so a 4D array isn't needed */
    ALIGNED_ARRAY_64( uint8_t, visited,[8],[8][8] );
    /* all permutations of an offset in up to 2 of the dimensions */
    ALIGNED_4( static const int8_t dia4d[33][4] ) =
    {
        {0,0,0,0},
        {0,0,0,1}, {0,0,0,-1}, {0,0,1,0}, {0,0,-1,0},
        {0,1,0,0}, {0,-1,0,0}, {1,0,0,0}, {-1,0,0,0},
        {0,0,1,1}, {0,0,-1,-1},{0,1,1,0}, {0,-1,-1,0},
        {1,1,0,0}, {-1,-1,0,0},{1,0,0,1}, {-1,0,0,-1},
        {0,1,0,1}, {0,-1,0,-1},{1,0,1,0}, {-1,0,-1,0},
        {0,0,-1,1},{0,0,1,-1}, {0,-1,1,0},{0,1,-1,0},
        {-1,1,0,0},{1,-1,0,0}, {1,0,0,-1},{-1,0,0,1},
        {0,-1,0,1},{0,1,0,-1}, {-1,0,1,0},{1,0,-1,0},
    };

    if( bm0y < h->mb.mv_min_spel[1] + 8 || bm1y < h->mb.mv_min_spel[1] + 8 ||
        bm0y > h->mb.mv_max_spel[1] - 8 || bm1y > h->mb.mv_max_spel[1] - 8 ||
        bm0x < h->mb.mv_min_spel[0] + 8 || bm1x < h->mb.mv_min_spel[0] + 8 ||
        bm0x > h->mb.mv_max_spel[0] - 8 || bm1x > h->mb.mv_max_spel[0] - 8 )
        return;

    if( rd && m0->i_pixel != PIXEL_16x16 && i8 != 0 )
    {
        x264_mb_predict_mv( h, 0, i8<<2, bw>>2, m0->mvp );
        x264_mb_predict_mv( h, 1, i8<<2, bw>>2, m1->mvp );
    }

    const uint16_t *p_cost_m0x = m0->p_cost_mv - m0->mvp[0];
    const uint16_t *p_cost_m0y = m0->p_cost_mv - m0->mvp[1];
    const uint16_t *p_cost_m1x = m1->p_cost_mv - m1->mvp[0];
    const uint16_t *p_cost_m1y = m1->p_cost_mv - m1->mvp[1];

    h->mc.memzero_aligned( visited, sizeof(uint8_t[8][8][8]) );

    for( int pass = 0; pass < 8; pass++ )
    {
        int bestj = 0;
        /* check all mv pairs that differ in at most 2 components from the current mvs. */
        /* doesn't do chroma ME. this probably doesn't matter, as the gains
         * from bidir ME are the same with and without chroma ME. */

        if( mc_list0 )
            for( int j = x264_iter_kludge; j < 9; j++ )
                BIME_CACHE( square1[j][0], square1[j][1], 0 );

        if( mc_list1 )
            for( int j = x264_iter_kludge; j < 9; j++ )
                BIME_CACHE( square1[j][0], square1[j][1], 1 );

        for( int j = !!pass; j < 33; j++ )
        {
            int m0x = dia4d[j][0] + bm0x;
            int m0y = dia4d[j][1] + bm0y;
            int m1x = dia4d[j][2] + bm1x;
            int m1y = dia4d[j][3] + bm1y;
            if( !pass || !((visited[(m0x)&7][(m0y)&7][(m1x)&7] & (1<<((m1y)&7)))) )
            {
                int i0 = 4 + 3*dia4d[j][0] + dia4d[j][1];
                int i1 = 4 + 3*dia4d[j][2] + dia4d[j][3];
                visited[(m0x)&7][(m0y)&7][(m1x)&7] |= (1<<((m1y)&7));
                h->mc.avg[i_pixel]( pix, FDEC_STRIDE, src[0][0][i0], stride[0][0][i0], src[0][1][i1], stride[0][1][i1], i_weight );
                int cost = h->pixf.mbcmp[i_pixel]( m0->p_fenc[0], FENC_STRIDE, pix, FDEC_STRIDE )
                         + p_cost_m0x[m0x] + p_cost_m0y[m0y] + p_cost_m1x[m1x] + p_cost_m1y[m1y];
                if( rd )
                {
                    if( cost < SATD_THRESH(bcost) )
                    {
                        bcost = X264_MIN( cost, bcost );
                        M32( cache0_mv ) = pack16to32_mask(m0x,m0y);
                        M32( cache1_mv ) = pack16to32_mask(m1x,m1y);
                        if( CHROMA444 )
                        {
                            h->mc.avg[i_pixel]( pixu, FDEC_STRIDE, src[1][0][i0], stride[1][0][i0], src[1][1][i1], stride[1][1][i1], i_weight );
                            h->mc.avg[i_pixel]( pixv, FDEC_STRIDE, src[2][0][i0], stride[2][0][i0], src[2][1][i1], stride[2][1][i1], i_weight );
                        }
                        else if( CHROMA_FORMAT )
                        {
                            h->mc.avg[chromapix]( pixu, FDEC_STRIDE, pixu_buf[0][i0], 8, pixu_buf[1][i1], 8, i_weight );
                            h->mc.avg[chromapix]( pixv, FDEC_STRIDE, pixv_buf[0][i0], 8, pixv_buf[1][i1], 8, i_weight );
                        }
                        uint64_t costrd = x264_rd_cost_part( h, i_lambda2, i8*4, m0->i_pixel );
                        COPY2_IF_LT( bcostrd, costrd, bestj, j );
                    }
                }
                else
                    COPY2_IF_LT( bcost, cost, bestj, j );
            }
        }

        if( !bestj )
            break;

        bm0x += dia4d[bestj][0];
        bm0y += dia4d[bestj][1];
        bm1x += dia4d[bestj][2];
        bm1y += dia4d[bestj][3];

        mc_list0 = M16( &dia4d[bestj][0] );
        mc_list1 = M16( &dia4d[bestj][2] );
    }

    if( rd )
    {
        x264_macroblock_cache_mv ( h, 2*x, 2*y, bw>>2, bh>>2, 0, pack16to32_mask(bm0x, bm0y) );
        amvd = pack8to16( X264_MIN(abs(bm0x - m0->mvp[0]),33), X264_MIN(abs(bm0y - m0->mvp[1]),33) );
        x264_macroblock_cache_mvd( h, 2*x, 2*y, bw>>2, bh>>2, 0, amvd );

        x264_macroblock_cache_mv ( h, 2*x, 2*y, bw>>2, bh>>2, 1, pack16to32_mask(bm1x, bm1y) );
        amvd = pack8to16( X264_MIN(abs(bm1x - m1->mvp[0]),33), X264_MIN(abs(bm1y - m1->mvp[1]),33) );
        x264_macroblock_cache_mvd( h, 2*x, 2*y, bw>>2, bh>>2, 1, amvd );
    }

    m0->mv[0] = bm0x;
    m0->mv[1] = bm0y;
    m1->mv[0] = bm1x;
    m1->mv[1] = bm1y;
}

void x264_me_refine_bidir_satd( x264_t *h, x264_me_t *m0, x264_me_t *m1, int i_weight )
{
    me_refine_bidir( h, m0, m1, i_weight, 0, 0, 0 );
}

void x264_me_refine_bidir_rd( x264_t *h, x264_me_t *m0, x264_me_t *m1, int i_weight, int i8, int i_lambda2 )
{
    /* Motion compensation is done as part of bidir_rd; don't repeat
     * it in encoding. */
    h->mb.b_skip_mc = 1;
    me_refine_bidir( h, m0, m1, i_weight, i8, i_lambda2, 1 );
    h->mb.b_skip_mc = 0;
}

#undef COST_MV_SATD
#define COST_MV_SATD( mx, my, dst, avoid_mvp ) \
{ \
    if( !avoid_mvp || !(mx == pmx && my == pmy) ) \
    { \
        h->mc.mc_luma( pix, FDEC_STRIDE, m->p_fref, m->i_stride[0], mx, my, bw, bh, &m->weight[0] ); \
        dst = h->pixf.mbcmp[i_pixel]( m->p_fenc[0], FENC_STRIDE, pix, FDEC_STRIDE ) \
            + p_cost_mvx[mx] + p_cost_mvy[my]; \
        COPY1_IF_LT( bsatd, dst ); \
    } \
    else \
        dst = COST_MAX; \
}

#define COST_MV_RD( mx, my, satd, do_dir, mdir ) \
{ \
    if( satd <= SATD_THRESH(bsatd) ) \
    { \
        uint64_t cost; \
        M32( cache_mv ) = pack16to32_mask(mx,my); \
        if( CHROMA444 ) \
        { \
            h->mc.mc_luma( pixu, FDEC_STRIDE, &m->p_fref[4], m->i_stride[1], mx, my, bw, bh, &m->weight[1] ); \
            h->mc.mc_luma( pixv, FDEC_STRIDE, &m->p_fref[8], m->i_stride[2], mx, my, bw, bh, &m->weight[2] ); \
        } \
        else if( CHROMA_FORMAT && m->i_pixel <= PIXEL_8x8 ) \
        { \
            h->mc.mc_chroma( pixu, pixv, FDEC_STRIDE, m->p_fref[4], m->i_stride[1], \
                             mx, 2*(my+mvy_offset)>>chroma_v_shift, bw>>1, bh>>chroma_v_shift ); \
            if( m->weight[1].weightfn ) \
                m->weight[1].weightfn[bw>>3]( pixu, FDEC_STRIDE, pixu, FDEC_STRIDE, &m->weight[1], bh>>chroma_v_shift ); \
            if( m->weight[2].weightfn ) \
                m->weight[2].weightfn[bw>>3]( pixv, FDEC_STRIDE, pixv, FDEC_STRIDE, &m->weight[2], bh>>chroma_v_shift ); \
        } \
        cost = x264_rd_cost_part( h, i_lambda2, i4, m->i_pixel ); \
        COPY4_IF_LT( bcost, cost, bmx, mx, bmy, my, dir, do_dir?mdir:dir ); \
    } \
}

void x264_me_refine_qpel_rd( x264_t *h, x264_me_t *m, int i_lambda2, int i4, int i_list )
{
    int16_t *cache_mv = h->mb.cache.mv[i_list][x264_scan8[i4]];
    const uint16_t *p_cost_mvx, *p_cost_mvy;
    const int bw = x264_pixel_size[m->i_pixel].w;
    const int bh = x264_pixel_size[m->i_pixel].h;
    const int i_pixel = m->i_pixel;
    int chroma_v_shift = CHROMA_V_SHIFT;
    int mvy_offset = chroma_v_shift & MB_INTERLACED & m->i_ref ? (h->mb.i_mb_y & 1)*4 - 2 : 0;

    uint64_t bcost = COST_MAX64;
    int bmx = m->mv[0];
    int bmy = m->mv[1];
    int omx, omy, pmx, pmy;
    int satd, bsatd;
    int dir = -2;
    int i8 = i4>>2;
    uint16_t amvd;

    pixel *pix  = &h->mb.pic.p_fdec[0][block_idx_xy_fdec[i4]];
    pixel *pixu, *pixv;
    if( CHROMA444 )
    {
        pixu = &h->mb.pic.p_fdec[1][block_idx_xy_fdec[i4]];
        pixv = &h->mb.pic.p_fdec[2][block_idx_xy_fdec[i4]];
    }
    else if( CHROMA_FORMAT )
    {
        pixu = &h->mb.pic.p_fdec[1][(i8>>1)*(8*FDEC_STRIDE>>chroma_v_shift)+(i8&1)*4];
        pixv = &h->mb.pic.p_fdec[2][(i8>>1)*(8*FDEC_STRIDE>>chroma_v_shift)+(i8&1)*4];
    }
    else
    {
        pixu = NULL;
        pixv = NULL;
    }

    h->mb.b_skip_mc = 1;

    if( m->i_pixel != PIXEL_16x16 && i4 != 0 )
        x264_mb_predict_mv( h, i_list, i4, bw>>2, m->mvp );
    pmx = m->mvp[0];
    pmy = m->mvp[1];
    p_cost_mvx = m->p_cost_mv - pmx;
    p_cost_mvy = m->p_cost_mv - pmy;
    COST_MV_SATD( bmx, bmy, bsatd, 0 );
    if( m->i_pixel != PIXEL_16x16 )
        COST_MV_RD( bmx, bmy, 0, 0, 0 )
    else
        bcost = m->cost;

    /* check the predicted mv */
    if( (bmx != pmx || bmy != pmy)
        && pmx >= h->mb.mv_min_spel[0] && pmx <= h->mb.mv_max_spel[0]
        && pmy >= h->mb.mv_min_spel[1] && pmy <= h->mb.mv_max_spel[1] )
    {
        COST_MV_SATD( pmx, pmy, satd, 0 );
        COST_MV_RD  ( pmx, pmy, satd, 0, 0 );
        /* The hex motion search is guaranteed to not repeat the center candidate,
         * so if pmv is chosen, set the "MV to avoid checking" to bmv instead. */
        if( bmx == pmx && bmy == pmy )
        {
            pmx = m->mv[0];
            pmy = m->mv[1];
        }
    }

    if( bmy < h->mb.mv_min_spel[1] + 3 || bmy > h->mb.mv_max_spel[1] - 3 ||
        bmx < h->mb.mv_min_spel[0] + 3 || bmx > h->mb.mv_max_spel[0] - 3 )
    {
        h->mb.b_skip_mc = 0;
        return;
    }

    /* subpel hex search, same pattern as ME HEX. */
    dir = -2;
    omx = bmx;
    omy = bmy;
    for( int j = 0; j < 6; j++ )
    {
        COST_MV_SATD( omx + hex2[j+1][0], omy + hex2[j+1][1], satd, 1 );
        COST_MV_RD  ( omx + hex2[j+1][0], omy + hex2[j+1][1], satd, 1, j );
    }

    if( dir != -2 )
    {
        /* half hexagon, not overlapping the previous iteration */
        for( int i = 1; i < 10; i++ )
        {
            const int odir = mod6m1[dir+1];
            if( bmy < h->mb.mv_min_spel[1] + 3 ||
                bmy > h->mb.mv_max_spel[1] - 3 )
                break;
            dir = -2;
            omx = bmx;
            omy = bmy;
            for( int j = 0; j < 3; j++ )
            {
                COST_MV_SATD( omx + hex2[odir+j][0], omy + hex2[odir+j][1], satd, 1 );
                COST_MV_RD  ( omx + hex2[odir+j][0], omy + hex2[odir+j][1], satd, 1, odir-1+j );
            }
            if( dir == -2 )
                break;
        }
    }

    /* square refine, same pattern as ME HEX. */
    omx = bmx;
    omy = bmy;
    for( int i = 0; i < 8; i++ )
    {
        COST_MV_SATD( omx + square1[i+1][0], omy + square1[i+1][1], satd, 1 );
        COST_MV_RD  ( omx + square1[i+1][0], omy + square1[i+1][1], satd, 0, 0 );
    }

    m->cost = bcost;
    m->mv[0] = bmx;
    m->mv[1] = bmy;
    x264_macroblock_cache_mv ( h, block_idx_x[i4], block_idx_y[i4], bw>>2, bh>>2, i_list, pack16to32_mask(bmx, bmy) );
    amvd = pack8to16( X264_MIN(abs(bmx - m->mvp[0]),66), X264_MIN(abs(bmy - m->mvp[1]),66) );
    x264_macroblock_cache_mvd( h, block_idx_x[i4], block_idx_y[i4], bw>>2, bh>>2, i_list, amvd );
    h->mb.b_skip_mc = 0;
}
