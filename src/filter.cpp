/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                           License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2000-2008, Intel Corporation, all rights reserved.
// Copyright (C) 2009, Willow Garage Inc., all rights reserved.
// Third party copyrights are property of their respective owners.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of the copyright holders may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#if CV_MAJOR_VERSION == 3
#include "filterengine.hpp"
#include <opencv2/imgproc/hal/hal.hpp>

#define  CV_MALLOC_ALIGN    16

namespace cv
{
void scalarToRawData(const Scalar& s, void* _buf, int type, int unroll_to)
{
    int i, depth = CV_MAT_DEPTH(type), cn = CV_MAT_CN(type);
    CV_Assert(cn <= 4);
    switch(depth)
    {
    case CV_8U:
        {
        uchar* buf = (uchar*)_buf;
        for(i = 0; i < cn; i++)
            buf[i] = saturate_cast<uchar>(s.val[i]);
        for(; i < unroll_to; i++)
            buf[i] = buf[i-cn];
        }
        break;
    case CV_8S:
        {
        schar* buf = (schar*)_buf;
        for(i = 0; i < cn; i++)
            buf[i] = saturate_cast<schar>(s.val[i]);
        for(; i < unroll_to; i++)
            buf[i] = buf[i-cn];
        }
        break;
    case CV_16U:
        {
        ushort* buf = (ushort*)_buf;
        for(i = 0; i < cn; i++)
            buf[i] = saturate_cast<ushort>(s.val[i]);
        for(; i < unroll_to; i++)
            buf[i] = buf[i-cn];
        }
        break;
    case CV_16S:
        {
        short* buf = (short*)_buf;
        for(i = 0; i < cn; i++)
            buf[i] = saturate_cast<short>(s.val[i]);
        for(; i < unroll_to; i++)
            buf[i] = buf[i-cn];
        }
        break;
    case CV_32S:
        {
        int* buf = (int*)_buf;
        for(i = 0; i < cn; i++)
            buf[i] = saturate_cast<int>(s.val[i]);
        for(; i < unroll_to; i++)
            buf[i] = buf[i-cn];
        }
        break;
    case CV_32F:
        {
        float* buf = (float*)_buf;
        for(i = 0; i < cn; i++)
            buf[i] = saturate_cast<float>(s.val[i]);
        for(; i < unroll_to; i++)
            buf[i] = buf[i-cn];
        }
        break;
    case CV_64F:
        {
        double* buf = (double*)_buf;
        for(i = 0; i < cn; i++)
            buf[i] = saturate_cast<double>(s.val[i]);
        for(; i < unroll_to; i++)
            buf[i] = buf[i-cn];
        break;
        }
    default:
        CV_Error(CV_StsUnsupportedFormat,"");
    }
}
}

/****************************************************************************************\
                                    Base Image Filter
\****************************************************************************************/

#if IPP_VERSION_X100 >= 710
#define USE_IPP_SEP_FILTERS 1
#else
#undef USE_IPP_SEP_FILTERS
#endif

namespace cv
{

BaseRowFilter::BaseRowFilter() { ksize = anchor = -1; }
BaseRowFilter::~BaseRowFilter() {}

BaseColumnFilter::BaseColumnFilter() { ksize = anchor = -1; }
BaseColumnFilter::~BaseColumnFilter() {}
void BaseColumnFilter::reset() {}

BaseFilter::BaseFilter() { ksize = Size(-1,-1); anchor = Point(-1,-1); }
BaseFilter::~BaseFilter() {}
void BaseFilter::reset() {}

FilterEngine::FilterEngine()
{
    srcType = dstType = bufType = -1;
    rowBorderType = columnBorderType = BORDER_REPLICATE;
    bufStep = startY = startY0 = endY = rowCount = dstY = 0;
    maxWidth = 0;

    wholeSize = Size(-1,-1);
}


FilterEngine::FilterEngine( const Ptr<BaseFilter>& _filter2D,
                            const Ptr<BaseRowFilter>& _rowFilter,
                            const Ptr<BaseColumnFilter>& _columnFilter,
                            int _srcType, int _dstType, int _bufType,
                            int _rowBorderType, int _columnBorderType,
                            const Scalar& _borderValue )
{
    init(_filter2D, _rowFilter, _columnFilter, _srcType, _dstType, _bufType,
         _rowBorderType, _columnBorderType, _borderValue);
}

FilterEngine::~FilterEngine()
{
}


void FilterEngine::init( const Ptr<BaseFilter>& _filter2D,
                         const Ptr<BaseRowFilter>& _rowFilter,
                         const Ptr<BaseColumnFilter>& _columnFilter,
                         int _srcType, int _dstType, int _bufType,
                         int _rowBorderType, int _columnBorderType,
                         const Scalar& _borderValue )
{
    _srcType = CV_MAT_TYPE(_srcType);
    _bufType = CV_MAT_TYPE(_bufType);
    _dstType = CV_MAT_TYPE(_dstType);

    srcType = _srcType;
    int srcElemSize = (int)getElemSize(srcType);
    dstType = _dstType;
    bufType = _bufType;

    filter2D = _filter2D;
    rowFilter = _rowFilter;
    columnFilter = _columnFilter;

    if( _columnBorderType < 0 )
        _columnBorderType = _rowBorderType;

    rowBorderType = _rowBorderType;
    columnBorderType = _columnBorderType;

    CV_Assert( columnBorderType != BORDER_WRAP );

    if( isSeparable() )
    {
        CV_Assert( rowFilter && columnFilter );
        ksize = Size(rowFilter->ksize, columnFilter->ksize);
        anchor = Point(rowFilter->anchor, columnFilter->anchor);
    }
    else
    {
        CV_Assert( bufType == srcType );
        ksize = filter2D->ksize;
        anchor = filter2D->anchor;
    }

    CV_Assert( 0 <= anchor.x && anchor.x < ksize.width &&
               0 <= anchor.y && anchor.y < ksize.height );

    borderElemSize = srcElemSize/(CV_MAT_DEPTH(srcType) >= CV_32S ? sizeof(int) : 1);
    int borderLength = std::max(ksize.width - 1, 1);
    borderTab.resize(borderLength*borderElemSize);

    maxWidth = bufStep = 0;
    constBorderRow.clear();

    if( rowBorderType == BORDER_CONSTANT || columnBorderType == BORDER_CONSTANT )
    {
        constBorderValue.resize(srcElemSize*borderLength);
        int srcType1 = CV_MAKETYPE(CV_MAT_DEPTH(srcType), MIN(CV_MAT_CN(srcType), 4));
        scalarToRawData(_borderValue, &constBorderValue[0], srcType1,
                        borderLength*CV_MAT_CN(srcType));
    }

    wholeSize = Size(-1,-1);
}

#define VEC_ALIGN CV_MALLOC_ALIGN

int FilterEngine::start(const Size &_wholeSize, const Size &sz, const Point &ofs)
{
    int i, j;

    wholeSize = _wholeSize;
    roi = Rect(ofs, sz);
    CV_Assert( roi.x >= 0 && roi.y >= 0 && roi.width >= 0 && roi.height >= 0 &&
        roi.x + roi.width <= wholeSize.width &&
        roi.y + roi.height <= wholeSize.height );

    int esz = (int)getElemSize(srcType);
    int bufElemSize = (int)getElemSize(bufType);
    const uchar* constVal = !constBorderValue.empty() ? &constBorderValue[0] : 0;

    int _maxBufRows = std::max(ksize.height + 3,
                               std::max(anchor.y,
                                        ksize.height-anchor.y-1)*2+1);

    if( maxWidth < roi.width || _maxBufRows != (int)rows.size() )
    {
        rows.resize(_maxBufRows);
        maxWidth = std::max(maxWidth, roi.width);
        int cn = CV_MAT_CN(srcType);
        srcRow.resize(esz*(maxWidth + ksize.width - 1));
        if( columnBorderType == BORDER_CONSTANT )
        {
            constBorderRow.resize(getElemSize(bufType)*(maxWidth + ksize.width - 1 + VEC_ALIGN));
            uchar *dst = alignPtr(&constBorderRow[0], VEC_ALIGN), *tdst;
            int n = (int)constBorderValue.size(), N;
            N = (maxWidth + ksize.width - 1)*esz;
            tdst = isSeparable() ? &srcRow[0] : dst;

            for( i = 0; i < N; i += n )
            {
                n = std::min( n, N - i );
                for(j = 0; j < n; j++)
                    tdst[i+j] = constVal[j];
            }

            if( isSeparable() )
                (*rowFilter)(&srcRow[0], dst, maxWidth, cn);
        }

        int maxBufStep = bufElemSize*(int)alignSize(maxWidth +
            (!isSeparable() ? ksize.width - 1 : 0),VEC_ALIGN);
        ringBuf.resize(maxBufStep*rows.size()+VEC_ALIGN);
    }

    // adjust bufstep so that the used part of the ring buffer stays compact in memory
    bufStep = bufElemSize*(int)alignSize(roi.width + (!isSeparable() ? ksize.width - 1 : 0),16);

    dx1 = std::max(anchor.x - roi.x, 0);
    dx2 = std::max(ksize.width - anchor.x - 1 + roi.x + roi.width - wholeSize.width, 0);

    // recompute border tables
    if( dx1 > 0 || dx2 > 0 )
    {
        if( rowBorderType == BORDER_CONSTANT )
        {
            int nr = isSeparable() ? 1 : (int)rows.size();
            for( i = 0; i < nr; i++ )
            {
                uchar* dst = isSeparable() ? &srcRow[0] : alignPtr(&ringBuf[0],VEC_ALIGN) + bufStep*i;
                memcpy( dst, constVal, dx1*esz );
                memcpy( dst + (roi.width + ksize.width - 1 - dx2)*esz, constVal, dx2*esz );
            }
        }
        else
        {
            int xofs1 = std::min(roi.x, anchor.x) - roi.x;

            int btab_esz = borderElemSize, wholeWidth = wholeSize.width;
            int* btab = (int*)&borderTab[0];

            for( i = 0; i < dx1; i++ )
            {
                int p0 = (borderInterpolate(i-dx1, wholeWidth, rowBorderType) + xofs1)*btab_esz;
                for( j = 0; j < btab_esz; j++ )
                    btab[i*btab_esz + j] = p0 + j;
            }

            for( i = 0; i < dx2; i++ )
            {
                int p0 = (borderInterpolate(wholeWidth + i, wholeWidth, rowBorderType) + xofs1)*btab_esz;
                for( j = 0; j < btab_esz; j++ )
                    btab[(i + dx1)*btab_esz + j] = p0 + j;
            }
        }
    }

    rowCount = dstY = 0;
    startY = startY0 = std::max(roi.y - anchor.y, 0);
    endY = std::min(roi.y + roi.height + ksize.height - anchor.y - 1, wholeSize.height);
    if( columnFilter )
        columnFilter->reset();
    if( filter2D )
        filter2D->reset();

    return startY;
}


int FilterEngine::start(const Mat& src, const Size &wsz, const Point &ofs)
{
    start( wsz, src.size(), ofs);
    return startY - ofs.y;
}

int FilterEngine::remainingInputRows() const
{
    return endY - startY - rowCount;
}

int FilterEngine::remainingOutputRows() const
{
    return roi.height - dstY;
}

int FilterEngine::proceed( const uchar* src, int srcstep, int count,
                           uchar* dst, int dststep )
{
    CV_Assert( wholeSize.width > 0 && wholeSize.height > 0 );

    const int *btab = &borderTab[0];
    int esz = (int)getElemSize(srcType), btab_esz = borderElemSize;
    uchar** brows = &rows[0];
    int bufRows = (int)rows.size();
    int cn = CV_MAT_CN(bufType);
    int width = roi.width, kwidth = ksize.width;
    int kheight = ksize.height, ay = anchor.y;
    int _dx1 = dx1, _dx2 = dx2;
    int width1 = roi.width + kwidth - 1;
    int xofs1 = std::min(roi.x, anchor.x);
    bool isSep = isSeparable();
    bool makeBorder = (_dx1 > 0 || _dx2 > 0) && rowBorderType != BORDER_CONSTANT;
    int dy = 0, i = 0;

    src -= xofs1*esz;
    count = std::min(count, remainingInputRows());

    CV_Assert( src && dst && count > 0 );

    for(;; dst += dststep*i, dy += i)
    {
        int dcount = bufRows - ay - startY - rowCount + roi.y;
        dcount = dcount > 0 ? dcount : bufRows - kheight + 1;
        dcount = std::min(dcount, count);
        count -= dcount;
        for( ; dcount-- > 0; src += srcstep )
        {
            int bi = (startY - startY0 + rowCount) % bufRows;
            uchar* brow = alignPtr(&ringBuf[0], VEC_ALIGN) + bi*bufStep;
            uchar* row = isSep ? &srcRow[0] : brow;

            if( ++rowCount > bufRows )
            {
                --rowCount;
                ++startY;
            }

            memcpy( row + _dx1*esz, src, (width1 - _dx2 - _dx1)*esz );

            if( makeBorder )
            {
                if( btab_esz*(int)sizeof(int) == esz )
                {
                    const int* isrc = (const int*)src;
                    int* irow = (int*)row;

                    for( i = 0; i < _dx1*btab_esz; i++ )
                        irow[i] = isrc[btab[i]];
                    for( i = 0; i < _dx2*btab_esz; i++ )
                        irow[i + (width1 - _dx2)*btab_esz] = isrc[btab[i+_dx1*btab_esz]];
                }
                else
                {
                    for( i = 0; i < _dx1*esz; i++ )
                        row[i] = src[btab[i]];
                    for( i = 0; i < _dx2*esz; i++ )
                        row[i + (width1 - _dx2)*esz] = src[btab[i+_dx1*esz]];
                }
            }

            if( isSep )
                (*rowFilter)(row, brow, width, CV_MAT_CN(srcType));
        }

        int max_i = std::min(bufRows, roi.height - (dstY + dy) + (kheight - 1));
        for( i = 0; i < max_i; i++ )
        {
            int srcY = borderInterpolate(dstY + dy + i + roi.y - ay,
                            wholeSize.height, columnBorderType);
            if( srcY < 0 ) // can happen only with constant border type
                brows[i] = alignPtr(&constBorderRow[0], VEC_ALIGN);
            else
            {
                CV_Assert( srcY >= startY );
                if( srcY >= startY + rowCount )
                    break;
                int bi = (srcY - startY0) % bufRows;
                brows[i] = alignPtr(&ringBuf[0], VEC_ALIGN) + bi*bufStep;
            }
        }
        if( i < kheight )
            break;
        i -= kheight - 1;
        if( isSeparable() )
            (*columnFilter)((const uchar**)brows, dst, dststep, i, roi.width*cn);
        else
            (*filter2D)((const uchar**)brows, dst, dststep, i, roi.width, cn);
    }

    dstY += dy;
    CV_Assert( dstY <= roi.height );
    return dy;
}

void FilterEngine::apply(const Mat& src, Mat& dst, const Size & wsz, const Point & ofs)
{
    

    CV_Assert( src.type() == srcType && dst.type() == dstType );

    int y = start(src, wsz, ofs);
    proceed(src.ptr() + y*src.step,
            (int)src.step,
            endY - startY,
            dst.ptr(),
            (int)dst.step );
}

}

/****************************************************************************************\
*                                 Separable linear filter                                *
\****************************************************************************************/

int cv::getKernelType(InputArray filter_kernel, Point anchor)
{
    Mat _kernel = filter_kernel.getMat();
    CV_Assert( _kernel.channels() == 1 );
    int i, sz = _kernel.rows*_kernel.cols;

    Mat kernel;
    _kernel.convertTo(kernel, CV_64F);

    const double* coeffs = kernel.ptr<double>();
    double sum = 0;
    int type = KERNEL_SMOOTH + KERNEL_INTEGER;
    if( (_kernel.rows == 1 || _kernel.cols == 1) &&
        anchor.x*2 + 1 == _kernel.cols &&
        anchor.y*2 + 1 == _kernel.rows )
        type |= (KERNEL_SYMMETRICAL + KERNEL_ASYMMETRICAL);

    for( i = 0; i < sz; i++ )
    {
        double a = coeffs[i], b = coeffs[sz - i - 1];
        if( a != b )
            type &= ~KERNEL_SYMMETRICAL;
        if( a != -b )
            type &= ~KERNEL_ASYMMETRICAL;
        if( a < 0 )
            type &= ~KERNEL_SMOOTH;
        if( a != saturate_cast<int>(a) )
            type &= ~KERNEL_INTEGER;
        sum += a;
    }

    if( fabs(sum - 1) > FLT_EPSILON*(fabs(sum) + 1) )
        type &= ~KERNEL_SMOOTH;
    return type;
}


namespace cv
{

struct RowNoVec
{
    RowNoVec() {}
    RowNoVec(const Mat&) {}
    int operator()(const uchar*, uchar*, int, int) const { return 0; }
};

struct ColumnNoVec
{
    ColumnNoVec() {}
    ColumnNoVec(const Mat&, int, int, double) {}
    int operator()(const uchar**, uchar*, int) const { return 0; }
};

struct SymmRowSmallNoVec
{
    SymmRowSmallNoVec() {}
    SymmRowSmallNoVec(const Mat&, int) {}
    int operator()(const uchar*, uchar*, int, int) const { return 0; }
};

struct SymmColumnSmallNoVec
{
    SymmColumnSmallNoVec() {}
    SymmColumnSmallNoVec(const Mat&, int, int, double) {}
    int operator()(const uchar**, uchar*, int) const { return 0; }
};

struct FilterNoVec
{
    FilterNoVec() {}
    FilterNoVec(const Mat&, int, double) {}
    int operator()(const uchar**, uchar*, int) const { return 0; }
};


#if CV_SSE2

///////////////////////////////////// 8u-16s & 8u-8u //////////////////////////////////

struct RowVec_8u32s
{
    RowVec_8u32s() { smallValues = false; }
    RowVec_8u32s( const Mat& _kernel )
    {
        kernel = _kernel;
        smallValues = true;
        int k, ksize = kernel.rows + kernel.cols - 1;
        for( k = 0; k < ksize; k++ )
        {
            int v = kernel.ptr<int>()[k];
            if( v < SHRT_MIN || v > SHRT_MAX )
            {
                smallValues = false;
                break;
            }
        }
    }

    int operator()(const uchar* _src, uchar* _dst, int width, int cn) const
    {
        if( !checkHardwareSupport(CV_CPU_SSE2) )
            return 0;

        int i = 0, k, _ksize = kernel.rows + kernel.cols - 1;
        int* dst = (int*)_dst;
        const int* _kx = kernel.ptr<int>();
        width *= cn;

        if( smallValues )
        {
            __m128i z = _mm_setzero_si128();
            for( ; i <= width - 8; i += 8 )
            {
                const uchar* src = _src + i;
                __m128i s0 = z, s1 = z;

                for( k = 0; k < _ksize; k++, src += cn )
                {
                    __m128i f = _mm_cvtsi32_si128(_kx[k]);
                    f = _mm_shuffle_epi32(f, 0);

                    __m128i x0 = _mm_loadl_epi64((const __m128i*)src);
                    x0 = _mm_unpacklo_epi8(x0, z);

                    __m128i x1 = _mm_unpackhi_epi16(x0, z);
                    x0 = _mm_unpacklo_epi16(x0, z);

                    x0 = _mm_madd_epi16(x0, f);
                    x1 = _mm_madd_epi16(x1, f);

                    s0 = _mm_add_epi32(s0, x0);
                    s1 = _mm_add_epi32(s1, x1);
                }

                _mm_store_si128((__m128i*)(dst + i), s0);
                _mm_store_si128((__m128i*)(dst + i + 4), s1);
            }

            if( i <= width - 4 )
            {
                const uchar* src = _src + i;
                __m128i s0 = z;

                for( k = 0; k < _ksize; k++, src += cn )
                {
                    __m128i f = _mm_cvtsi32_si128(_kx[k]);
                    f = _mm_shuffle_epi32(f, 0);

                    __m128i x0 = _mm_cvtsi32_si128(*(const int*)src);
                    x0 = _mm_unpacklo_epi8(x0, z);
                    x0 = _mm_unpacklo_epi16(x0, z);
                    x0 = _mm_madd_epi16(x0, f);
                    s0 = _mm_add_epi32(s0, x0);
                }
                _mm_store_si128((__m128i*)(dst + i), s0);
                i += 4;
            }
        }
        return i;
    }

    Mat kernel;
    bool smallValues;
};


struct SymmRowSmallVec_8u32s
{
    SymmRowSmallVec_8u32s() { smallValues = false; }
    SymmRowSmallVec_8u32s( const Mat& _kernel, int _symmetryType )
    {
        kernel = _kernel;
        symmetryType = _symmetryType;
        smallValues = true;
        int k, ksize = kernel.rows + kernel.cols - 1;
        for( k = 0; k < ksize; k++ )
        {
            int v = kernel.ptr<int>()[k];
            if( v < SHRT_MIN || v > SHRT_MAX )
            {
                smallValues = false;
                break;
            }
        }
    }

    int operator()(const uchar* src, uchar* _dst, int width, int cn) const
    {
        if( !checkHardwareSupport(CV_CPU_SSE2) )
            return 0;

        int i = 0, j, k, _ksize = kernel.rows + kernel.cols - 1;
        int* dst = (int*)_dst;
        bool symmetrical = (symmetryType & KERNEL_SYMMETRICAL) != 0;
        const int* kx = kernel.ptr<int>() + _ksize/2;
        if( !smallValues )
            return 0;

        src += (_ksize/2)*cn;
        width *= cn;

        __m128i z = _mm_setzero_si128();
        if( symmetrical )
        {
            if( _ksize == 1 )
                return 0;
            if( _ksize == 3 )
            {
                if( kx[0] == 2 && kx[1] == 1 )
                    for( ; i <= width - 16; i += 16, src += 16 )
                    {
                        __m128i x0, x1, x2, y0, y1, y2;
                        x0 = _mm_loadu_si128((__m128i*)(src - cn));
                        x1 = _mm_loadu_si128((__m128i*)src);
                        x2 = _mm_loadu_si128((__m128i*)(src + cn));
                        y0 = _mm_unpackhi_epi8(x0, z);
                        x0 = _mm_unpacklo_epi8(x0, z);
                        y1 = _mm_unpackhi_epi8(x1, z);
                        x1 = _mm_unpacklo_epi8(x1, z);
                        y2 = _mm_unpackhi_epi8(x2, z);
                        x2 = _mm_unpacklo_epi8(x2, z);
                        x0 = _mm_add_epi16(x0, _mm_add_epi16(_mm_add_epi16(x1, x1), x2));
                        y0 = _mm_add_epi16(y0, _mm_add_epi16(_mm_add_epi16(y1, y1), y2));
                        _mm_store_si128((__m128i*)(dst + i), _mm_unpacklo_epi16(x0, z));
                        _mm_store_si128((__m128i*)(dst + i + 4), _mm_unpackhi_epi16(x0, z));
                        _mm_store_si128((__m128i*)(dst + i + 8), _mm_unpacklo_epi16(y0, z));
                        _mm_store_si128((__m128i*)(dst + i + 12), _mm_unpackhi_epi16(y0, z));
                    }
                else if( kx[0] == -2 && kx[1] == 1 )
                    for( ; i <= width - 16; i += 16, src += 16 )
                    {
                        __m128i x0, x1, x2, y0, y1, y2;
                        x0 = _mm_loadu_si128((__m128i*)(src - cn));
                        x1 = _mm_loadu_si128((__m128i*)src);
                        x2 = _mm_loadu_si128((__m128i*)(src + cn));
                        y0 = _mm_unpackhi_epi8(x0, z);
                        x0 = _mm_unpacklo_epi8(x0, z);
                        y1 = _mm_unpackhi_epi8(x1, z);
                        x1 = _mm_unpacklo_epi8(x1, z);
                        y2 = _mm_unpackhi_epi8(x2, z);
                        x2 = _mm_unpacklo_epi8(x2, z);
                        x0 = _mm_add_epi16(x0, _mm_sub_epi16(x2, _mm_add_epi16(x1, x1)));
                        y0 = _mm_add_epi16(y0, _mm_sub_epi16(y2, _mm_add_epi16(y1, y1)));
                        _mm_store_si128((__m128i*)(dst + i), _mm_srai_epi32(_mm_unpacklo_epi16(x0, x0),16));
                        _mm_store_si128((__m128i*)(dst + i + 4), _mm_srai_epi32(_mm_unpackhi_epi16(x0, x0),16));
                        _mm_store_si128((__m128i*)(dst + i + 8), _mm_srai_epi32(_mm_unpacklo_epi16(y0, y0),16));
                        _mm_store_si128((__m128i*)(dst + i + 12), _mm_srai_epi32(_mm_unpackhi_epi16(y0, y0),16));
                    }
                else
                {
                    __m128i k0 = _mm_shuffle_epi32(_mm_cvtsi32_si128(kx[0]), 0),
                            k1 = _mm_shuffle_epi32(_mm_cvtsi32_si128(kx[1]), 0);
                    k1 = _mm_packs_epi32(k1, k1);

                    for( ; i <= width - 8; i += 8, src += 8 )
                    {
                        __m128i x0 = _mm_loadl_epi64((__m128i*)(src - cn));
                        __m128i x1 = _mm_loadl_epi64((__m128i*)src);
                        __m128i x2 = _mm_loadl_epi64((__m128i*)(src + cn));

                        x0 = _mm_unpacklo_epi8(x0, z);
                        x1 = _mm_unpacklo_epi8(x1, z);
                        x2 = _mm_unpacklo_epi8(x2, z);
                        __m128i x3 = _mm_unpacklo_epi16(x0, x2);
                        __m128i x4 = _mm_unpackhi_epi16(x0, x2);
                        __m128i x5 = _mm_unpacklo_epi16(x1, z);
                        __m128i x6 = _mm_unpackhi_epi16(x1, z);
                        x3 = _mm_madd_epi16(x3, k1);
                        x4 = _mm_madd_epi16(x4, k1);
                        x5 = _mm_madd_epi16(x5, k0);
                        x6 = _mm_madd_epi16(x6, k0);
                        x3 = _mm_add_epi32(x3, x5);
                        x4 = _mm_add_epi32(x4, x6);

                        _mm_store_si128((__m128i*)(dst + i), x3);
                        _mm_store_si128((__m128i*)(dst + i + 4), x4);
                    }
                }
            }
            else if( _ksize == 5 )
            {
                if( kx[0] == -2 && kx[1] == 0 && kx[2] == 1 )
                    for( ; i <= width - 16; i += 16, src += 16 )
                    {
                        __m128i x0, x1, x2, y0, y1, y2;
                        x0 = _mm_loadu_si128((__m128i*)(src - cn*2));
                        x1 = _mm_loadu_si128((__m128i*)src);
                        x2 = _mm_loadu_si128((__m128i*)(src + cn*2));
                        y0 = _mm_unpackhi_epi8(x0, z);
                        x0 = _mm_unpacklo_epi8(x0, z);
                        y1 = _mm_unpackhi_epi8(x1, z);
                        x1 = _mm_unpacklo_epi8(x1, z);
                        y2 = _mm_unpackhi_epi8(x2, z);
                        x2 = _mm_unpacklo_epi8(x2, z);
                        x0 = _mm_add_epi16(x0, _mm_sub_epi16(x2, _mm_add_epi16(x1, x1)));
                        y0 = _mm_add_epi16(y0, _mm_sub_epi16(y2, _mm_add_epi16(y1, y1)));
                        _mm_store_si128((__m128i*)(dst + i), _mm_srai_epi32(_mm_unpacklo_epi16(x0, x0),16));
                        _mm_store_si128((__m128i*)(dst + i + 4), _mm_srai_epi32(_mm_unpackhi_epi16(x0, x0),16));
                        _mm_store_si128((__m128i*)(dst + i + 8), _mm_srai_epi32(_mm_unpacklo_epi16(y0, y0),16));
                        _mm_store_si128((__m128i*)(dst + i + 12), _mm_srai_epi32(_mm_unpackhi_epi16(y0, y0),16));
                    }
                else
                {
                    __m128i k0 = _mm_shuffle_epi32(_mm_cvtsi32_si128(kx[0]), 0),
                            k1 = _mm_shuffle_epi32(_mm_cvtsi32_si128(kx[1]), 0),
                            k2 = _mm_shuffle_epi32(_mm_cvtsi32_si128(kx[2]), 0);
                    k1 = _mm_packs_epi32(k1, k1);
                    k2 = _mm_packs_epi32(k2, k2);

                    for( ; i <= width - 8; i += 8, src += 8 )
                    {
                        __m128i x0 = _mm_loadl_epi64((__m128i*)src);

                        x0 = _mm_unpacklo_epi8(x0, z);
                        __m128i x1 = _mm_unpacklo_epi16(x0, z);
                        __m128i x2 = _mm_unpackhi_epi16(x0, z);
                        x1 = _mm_madd_epi16(x1, k0);
                        x2 = _mm_madd_epi16(x2, k0);

                        __m128i x3 = _mm_loadl_epi64((__m128i*)(src - cn));
                        __m128i x4 = _mm_loadl_epi64((__m128i*)(src + cn));

                        x3 = _mm_unpacklo_epi8(x3, z);
                        x4 = _mm_unpacklo_epi8(x4, z);
                        __m128i x5 = _mm_unpacklo_epi16(x3, x4);
                        __m128i x6 = _mm_unpackhi_epi16(x3, x4);
                        x5 = _mm_madd_epi16(x5, k1);
                        x6 = _mm_madd_epi16(x6, k1);
                        x1 = _mm_add_epi32(x1, x5);
                        x2 = _mm_add_epi32(x2, x6);

                        x3 = _mm_loadl_epi64((__m128i*)(src - cn*2));
                        x4 = _mm_loadl_epi64((__m128i*)(src + cn*2));

                        x3 = _mm_unpacklo_epi8(x3, z);
                        x4 = _mm_unpacklo_epi8(x4, z);
                        x5 = _mm_unpacklo_epi16(x3, x4);
                        x6 = _mm_unpackhi_epi16(x3, x4);
                        x5 = _mm_madd_epi16(x5, k2);
                        x6 = _mm_madd_epi16(x6, k2);
                        x1 = _mm_add_epi32(x1, x5);
                        x2 = _mm_add_epi32(x2, x6);

                        _mm_store_si128((__m128i*)(dst + i), x1);
                        _mm_store_si128((__m128i*)(dst + i + 4), x2);
                    }
                }
            }
        }
        else
        {
            if( _ksize == 3 )
            {
                if( kx[0] == 0 && kx[1] == 1 )
                    for( ; i <= width - 16; i += 16, src += 16 )
                    {
                        __m128i x0, x1, y0;
                        x0 = _mm_loadu_si128((__m128i*)(src + cn));
                        x1 = _mm_loadu_si128((__m128i*)(src - cn));
                        y0 = _mm_sub_epi16(_mm_unpackhi_epi8(x0, z), _mm_unpackhi_epi8(x1, z));
                        x0 = _mm_sub_epi16(_mm_unpacklo_epi8(x0, z), _mm_unpacklo_epi8(x1, z));
                        _mm_store_si128((__m128i*)(dst + i), _mm_srai_epi32(_mm_unpacklo_epi16(x0, x0),16));
                        _mm_store_si128((__m128i*)(dst + i + 4), _mm_srai_epi32(_mm_unpackhi_epi16(x0, x0),16));
                        _mm_store_si128((__m128i*)(dst + i + 8), _mm_srai_epi32(_mm_unpacklo_epi16(y0, y0),16));
                        _mm_store_si128((__m128i*)(dst + i + 12), _mm_srai_epi32(_mm_unpackhi_epi16(y0, y0),16));
                    }
                else
                {
                    __m128i k0 = _mm_set_epi32(-kx[1], kx[1], -kx[1], kx[1]);
                    k0 = _mm_packs_epi32(k0, k0);

                    for( ; i <= width - 16; i += 16, src += 16 )
                    {
                        __m128i x0 = _mm_loadu_si128((__m128i*)(src + cn));
                        __m128i x1 = _mm_loadu_si128((__m128i*)(src - cn));

                        __m128i x2 = _mm_unpacklo_epi8(x0, z);
                        __m128i x3 = _mm_unpacklo_epi8(x1, z);
                        __m128i x4 = _mm_unpackhi_epi8(x0, z);
                        __m128i x5 = _mm_unpackhi_epi8(x1, z);
                        __m128i x6 = _mm_unpacklo_epi16(x2, x3);
                        __m128i x7 = _mm_unpacklo_epi16(x4, x5);
                        __m128i x8 = _mm_unpackhi_epi16(x2, x3);
                        __m128i x9 = _mm_unpackhi_epi16(x4, x5);
                        x6 = _mm_madd_epi16(x6, k0);
                        x7 = _mm_madd_epi16(x7, k0);
                        x8 = _mm_madd_epi16(x8, k0);
                        x9 = _mm_madd_epi16(x9, k0);

                        _mm_store_si128((__m128i*)(dst + i), x6);
                        _mm_store_si128((__m128i*)(dst + i + 4), x8);
                        _mm_store_si128((__m128i*)(dst + i + 8), x7);
                        _mm_store_si128((__m128i*)(dst + i + 12), x9);
                    }
                }
            }
            else if( _ksize == 5 )
            {
                __m128i k0 = _mm_loadl_epi64((__m128i*)(kx + 1));
                k0 = _mm_unpacklo_epi64(k0, k0);
                k0 = _mm_packs_epi32(k0, k0);

                for( ; i <= width - 16; i += 16, src += 16 )
                {
                    __m128i x0 = _mm_loadu_si128((__m128i*)(src + cn));
                    __m128i x1 = _mm_loadu_si128((__m128i*)(src - cn));

                    __m128i x2 = _mm_unpackhi_epi8(x0, z);
                    __m128i x3 = _mm_unpackhi_epi8(x1, z);
                    x0 = _mm_unpacklo_epi8(x0, z);
                    x1 = _mm_unpacklo_epi8(x1, z);
                    __m128i x5 = _mm_sub_epi16(x2, x3);
                    __m128i x4 = _mm_sub_epi16(x0, x1);

                    __m128i x6 = _mm_loadu_si128((__m128i*)(src + cn * 2));
                    __m128i x7 = _mm_loadu_si128((__m128i*)(src - cn * 2));

                    __m128i x8 = _mm_unpackhi_epi8(x6, z);
                    __m128i x9 = _mm_unpackhi_epi8(x7, z);
                    x6 = _mm_unpacklo_epi8(x6, z);
                    x7 = _mm_unpacklo_epi8(x7, z);
                    __m128i x11 = _mm_sub_epi16(x8, x9);
                    __m128i x10 = _mm_sub_epi16(x6, x7);

                    __m128i x13 = _mm_unpackhi_epi16(x5, x11);
                    __m128i x12 = _mm_unpackhi_epi16(x4, x10);
                    x5 = _mm_unpacklo_epi16(x5, x11);
                    x4 = _mm_unpacklo_epi16(x4, x10);
                    x5 = _mm_madd_epi16(x5, k0);
                    x4 = _mm_madd_epi16(x4, k0);
                    x13 = _mm_madd_epi16(x13, k0);
                    x12 = _mm_madd_epi16(x12, k0);

                    _mm_store_si128((__m128i*)(dst + i), x4);
                    _mm_store_si128((__m128i*)(dst + i + 4), x12);
                    _mm_store_si128((__m128i*)(dst + i + 8), x5);
                    _mm_store_si128((__m128i*)(dst + i + 12), x13);
                }
            }
        }

        src -= (_ksize/2)*cn;
        kx -= _ksize/2;
        for( ; i <= width - 4; i += 4, src += 4 )
        {
            __m128i s0 = z;

            for( k = j = 0; k < _ksize; k++, j += cn )
            {
                __m128i f = _mm_cvtsi32_si128(kx[k]);
                f = _mm_shuffle_epi32(f, 0);

                __m128i x0 = _mm_cvtsi32_si128(*(const int*)(src + j));
                x0 = _mm_unpacklo_epi8(x0, z);
                x0 = _mm_unpacklo_epi16(x0, z);
                x0 = _mm_madd_epi16(x0, f);
                s0 = _mm_add_epi32(s0, x0);
            }
            _mm_store_si128((__m128i*)(dst + i), s0);
        }

        return i;
    }

    Mat kernel;
    int symmetryType;
    bool smallValues;
};


struct SymmColumnVec_32s8u
{
    SymmColumnVec_32s8u() { symmetryType=0; }
    SymmColumnVec_32s8u(const Mat& _kernel, int _symmetryType, int _bits, double _delta)
    {
        symmetryType = _symmetryType;
        _kernel.convertTo(kernel, CV_32F, 1./(1 << _bits), 0);
        delta = (float)(_delta/(1 << _bits));
        CV_Assert( (symmetryType & (KERNEL_SYMMETRICAL | KERNEL_ASYMMETRICAL)) != 0 );
    }

    int operator()(const uchar** _src, uchar* dst, int width) const
    {
        if( !checkHardwareSupport(CV_CPU_SSE2) )
            return 0;

        int ksize2 = (kernel.rows + kernel.cols - 1)/2;
        const float* ky = kernel.ptr<float>() + ksize2;
        int i = 0, k;
        bool symmetrical = (symmetryType & KERNEL_SYMMETRICAL) != 0;
        const int** src = (const int**)_src;
        const __m128i *S, *S2;
        __m128 d4 = _mm_set1_ps(delta);

        if( symmetrical )
        {
            for( ; i <= width - 16; i += 16 )
            {
                __m128 f = _mm_load_ss(ky);
                f = _mm_shuffle_ps(f, f, 0);
                __m128 s0, s1, s2, s3;
                __m128i x0, x1;
                S = (const __m128i*)(src[0] + i);
                s0 = _mm_cvtepi32_ps(_mm_load_si128(S));
                s1 = _mm_cvtepi32_ps(_mm_load_si128(S+1));
                s0 = _mm_add_ps(_mm_mul_ps(s0, f), d4);
                s1 = _mm_add_ps(_mm_mul_ps(s1, f), d4);
                s2 = _mm_cvtepi32_ps(_mm_load_si128(S+2));
                s3 = _mm_cvtepi32_ps(_mm_load_si128(S+3));
                s2 = _mm_add_ps(_mm_mul_ps(s2, f), d4);
                s3 = _mm_add_ps(_mm_mul_ps(s3, f), d4);

                for( k = 1; k <= ksize2; k++ )
                {
                    S = (const __m128i*)(src[k] + i);
                    S2 = (const __m128i*)(src[-k] + i);
                    f = _mm_load_ss(ky+k);
                    f = _mm_shuffle_ps(f, f, 0);
                    x0 = _mm_add_epi32(_mm_load_si128(S), _mm_load_si128(S2));
                    x1 = _mm_add_epi32(_mm_load_si128(S+1), _mm_load_si128(S2+1));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_cvtepi32_ps(x0), f));
                    s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_cvtepi32_ps(x1), f));
                    x0 = _mm_add_epi32(_mm_load_si128(S+2), _mm_load_si128(S2+2));
                    x1 = _mm_add_epi32(_mm_load_si128(S+3), _mm_load_si128(S2+3));
                    s2 = _mm_add_ps(s2, _mm_mul_ps(_mm_cvtepi32_ps(x0), f));
                    s3 = _mm_add_ps(s3, _mm_mul_ps(_mm_cvtepi32_ps(x1), f));
                }

                x0 = _mm_packs_epi32(_mm_cvtps_epi32(s0), _mm_cvtps_epi32(s1));
                x1 = _mm_packs_epi32(_mm_cvtps_epi32(s2), _mm_cvtps_epi32(s3));
                x0 = _mm_packus_epi16(x0, x1);
                _mm_storeu_si128((__m128i*)(dst + i), x0);
            }

            for( ; i <= width - 4; i += 4 )
            {
                __m128 f = _mm_load_ss(ky);
                f = _mm_shuffle_ps(f, f, 0);
                __m128i x0;
                __m128 s0 = _mm_cvtepi32_ps(_mm_load_si128((const __m128i*)(src[0] + i)));
                s0 = _mm_add_ps(_mm_mul_ps(s0, f), d4);

                for( k = 1; k <= ksize2; k++ )
                {
                    S = (const __m128i*)(src[k] + i);
                    S2 = (const __m128i*)(src[-k] + i);
                    f = _mm_load_ss(ky+k);
                    f = _mm_shuffle_ps(f, f, 0);
                    x0 = _mm_add_epi32(_mm_load_si128(S), _mm_load_si128(S2));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_cvtepi32_ps(x0), f));
                }

                x0 = _mm_cvtps_epi32(s0);
                x0 = _mm_packs_epi32(x0, x0);
                x0 = _mm_packus_epi16(x0, x0);
                *(int*)(dst + i) = _mm_cvtsi128_si32(x0);
            }
        }
        else
        {
            for( ; i <= width - 16; i += 16 )
            {
                __m128 f, s0 = d4, s1 = d4, s2 = d4, s3 = d4;
                __m128i x0, x1;

                for( k = 1; k <= ksize2; k++ )
                {
                    S = (const __m128i*)(src[k] + i);
                    S2 = (const __m128i*)(src[-k] + i);
                    f = _mm_load_ss(ky+k);
                    f = _mm_shuffle_ps(f, f, 0);
                    x0 = _mm_sub_epi32(_mm_load_si128(S), _mm_load_si128(S2));
                    x1 = _mm_sub_epi32(_mm_load_si128(S+1), _mm_load_si128(S2+1));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_cvtepi32_ps(x0), f));
                    s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_cvtepi32_ps(x1), f));
                    x0 = _mm_sub_epi32(_mm_load_si128(S+2), _mm_load_si128(S2+2));
                    x1 = _mm_sub_epi32(_mm_load_si128(S+3), _mm_load_si128(S2+3));
                    s2 = _mm_add_ps(s2, _mm_mul_ps(_mm_cvtepi32_ps(x0), f));
                    s3 = _mm_add_ps(s3, _mm_mul_ps(_mm_cvtepi32_ps(x1), f));
                }

                x0 = _mm_packs_epi32(_mm_cvtps_epi32(s0), _mm_cvtps_epi32(s1));
                x1 = _mm_packs_epi32(_mm_cvtps_epi32(s2), _mm_cvtps_epi32(s3));
                x0 = _mm_packus_epi16(x0, x1);
                _mm_storeu_si128((__m128i*)(dst + i), x0);
            }

            for( ; i <= width - 4; i += 4 )
            {
                __m128 f, s0 = d4;
                __m128i x0;

                for( k = 1; k <= ksize2; k++ )
                {
                    S = (const __m128i*)(src[k] + i);
                    S2 = (const __m128i*)(src[-k] + i);
                    f = _mm_load_ss(ky+k);
                    f = _mm_shuffle_ps(f, f, 0);
                    x0 = _mm_sub_epi32(_mm_load_si128(S), _mm_load_si128(S2));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_cvtepi32_ps(x0), f));
                }

                x0 = _mm_cvtps_epi32(s0);
                x0 = _mm_packs_epi32(x0, x0);
                x0 = _mm_packus_epi16(x0, x0);
                *(int*)(dst + i) = _mm_cvtsi128_si32(x0);
            }
        }

        return i;
    }

    int symmetryType;
    float delta;
    Mat kernel;
};


struct SymmColumnSmallVec_32s16s
{
    SymmColumnSmallVec_32s16s() { symmetryType=0; }
    SymmColumnSmallVec_32s16s(const Mat& _kernel, int _symmetryType, int _bits, double _delta)
    {
        symmetryType = _symmetryType;
        _kernel.convertTo(kernel, CV_32F, 1./(1 << _bits), 0);
        delta = (float)(_delta/(1 << _bits));
        CV_Assert( (symmetryType & (KERNEL_SYMMETRICAL | KERNEL_ASYMMETRICAL)) != 0 );
    }

    int operator()(const uchar** _src, uchar* _dst, int width) const
    {
        if( !checkHardwareSupport(CV_CPU_SSE2) )
            return 0;

        int ksize2 = (kernel.rows + kernel.cols - 1)/2;
        const float* ky = kernel.ptr<float>() + ksize2;
        int i = 0;
        bool symmetrical = (symmetryType & KERNEL_SYMMETRICAL) != 0;
        const int** src = (const int**)_src;
        const int *S0 = src[-1], *S1 = src[0], *S2 = src[1];
        short* dst = (short*)_dst;
        __m128 df4 = _mm_set1_ps(delta);
        __m128i d4 = _mm_cvtps_epi32(df4);

        if( symmetrical )
        {
            if( ky[0] == 2 && ky[1] == 1 )
            {
                for( ; i <= width - 8; i += 8 )
                {
                    __m128i s0, s1, s2, s3, s4, s5;
                    s0 = _mm_load_si128((__m128i*)(S0 + i));
                    s1 = _mm_load_si128((__m128i*)(S0 + i + 4));
                    s2 = _mm_load_si128((__m128i*)(S1 + i));
                    s3 = _mm_load_si128((__m128i*)(S1 + i + 4));
                    s4 = _mm_load_si128((__m128i*)(S2 + i));
                    s5 = _mm_load_si128((__m128i*)(S2 + i + 4));
                    s0 = _mm_add_epi32(s0, _mm_add_epi32(s4, _mm_add_epi32(s2, s2)));
                    s1 = _mm_add_epi32(s1, _mm_add_epi32(s5, _mm_add_epi32(s3, s3)));
                    s0 = _mm_add_epi32(s0, d4);
                    s1 = _mm_add_epi32(s1, d4);
                    _mm_storeu_si128((__m128i*)(dst + i), _mm_packs_epi32(s0, s1));
                }
            }
            else if( ky[0] == -2 && ky[1] == 1 )
            {
                for( ; i <= width - 8; i += 8 )
                {
                    __m128i s0, s1, s2, s3, s4, s5;
                    s0 = _mm_load_si128((__m128i*)(S0 + i));
                    s1 = _mm_load_si128((__m128i*)(S0 + i + 4));
                    s2 = _mm_load_si128((__m128i*)(S1 + i));
                    s3 = _mm_load_si128((__m128i*)(S1 + i + 4));
                    s4 = _mm_load_si128((__m128i*)(S2 + i));
                    s5 = _mm_load_si128((__m128i*)(S2 + i + 4));
                    s0 = _mm_add_epi32(s0, _mm_sub_epi32(s4, _mm_add_epi32(s2, s2)));
                    s1 = _mm_add_epi32(s1, _mm_sub_epi32(s5, _mm_add_epi32(s3, s3)));
                    s0 = _mm_add_epi32(s0, d4);
                    s1 = _mm_add_epi32(s1, d4);
                    _mm_storeu_si128((__m128i*)(dst + i), _mm_packs_epi32(s0, s1));
                }
            }
            else
            {
                __m128 k0 = _mm_set1_ps(ky[0]), k1 = _mm_set1_ps(ky[1]);
                for( ; i <= width - 8; i += 8 )
                {
                    __m128 s0, s1;
                    s0 = _mm_cvtepi32_ps(_mm_load_si128((__m128i*)(S1 + i)));
                    s1 = _mm_cvtepi32_ps(_mm_load_si128((__m128i*)(S1 + i + 4)));
                    s0 = _mm_add_ps(_mm_mul_ps(s0, k0), df4);
                    s1 = _mm_add_ps(_mm_mul_ps(s1, k0), df4);
                    __m128i x0, x1;
                    x0 = _mm_add_epi32(_mm_load_si128((__m128i*)(S0 + i)),
                                       _mm_load_si128((__m128i*)(S2 + i)));
                    x1 = _mm_add_epi32(_mm_load_si128((__m128i*)(S0 + i + 4)),
                                       _mm_load_si128((__m128i*)(S2 + i + 4)));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_cvtepi32_ps(x0),k1));
                    s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_cvtepi32_ps(x1),k1));
                    x0 = _mm_packs_epi32(_mm_cvtps_epi32(s0), _mm_cvtps_epi32(s1));
                    _mm_storeu_si128((__m128i*)(dst + i), x0);
                }
            }
        }
        else
        {
            if( fabs(ky[1]) == 1 && ky[1] == -ky[-1] )
            {
                if( ky[1] < 0 )
                    std::swap(S0, S2);
                for( ; i <= width - 8; i += 8 )
                {
                    __m128i s0, s1, s2, s3;
                    s0 = _mm_load_si128((__m128i*)(S2 + i));
                    s1 = _mm_load_si128((__m128i*)(S2 + i + 4));
                    s2 = _mm_load_si128((__m128i*)(S0 + i));
                    s3 = _mm_load_si128((__m128i*)(S0 + i + 4));
                    s0 = _mm_add_epi32(_mm_sub_epi32(s0, s2), d4);
                    s1 = _mm_add_epi32(_mm_sub_epi32(s1, s3), d4);
                    _mm_storeu_si128((__m128i*)(dst + i), _mm_packs_epi32(s0, s1));
                }
            }
            else
            {
                __m128 k1 = _mm_set1_ps(ky[1]);
                for( ; i <= width - 8; i += 8 )
                {
                    __m128 s0 = df4, s1 = df4;
                    __m128i x0, x1;
                    x0 = _mm_sub_epi32(_mm_load_si128((__m128i*)(S2 + i)),
                                       _mm_load_si128((__m128i*)(S0 + i)));
                    x1 = _mm_sub_epi32(_mm_load_si128((__m128i*)(S2 + i + 4)),
                                       _mm_load_si128((__m128i*)(S0 + i + 4)));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_cvtepi32_ps(x0),k1));
                    s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_cvtepi32_ps(x1),k1));
                    x0 = _mm_packs_epi32(_mm_cvtps_epi32(s0), _mm_cvtps_epi32(s1));
                    _mm_storeu_si128((__m128i*)(dst + i), x0);
                }
            }
        }

        return i;
    }

    int symmetryType;
    float delta;
    Mat kernel;
};


/////////////////////////////////////// 16s //////////////////////////////////

struct RowVec_16s32f
{
    RowVec_16s32f() {}
    RowVec_16s32f( const Mat& _kernel )
    {
        kernel = _kernel;
        sse2_supported = checkHardwareSupport(CV_CPU_SSE2);
    }

    int operator()(const uchar* _src, uchar* _dst, int width, int cn) const
    {
        if( !sse2_supported )
            return 0;

        int i = 0, k, _ksize = kernel.rows + kernel.cols - 1;
        float* dst = (float*)_dst;
        const float* _kx = kernel.ptr<float>();
        width *= cn;

        for( ; i <= width - 8; i += 8 )
        {
            const short* src = (const short*)_src + i;
            __m128 f, s0 = _mm_setzero_ps(), s1 = s0, x0, x1;
            for( k = 0; k < _ksize; k++, src += cn )
            {
                f = _mm_load_ss(_kx+k);
                f = _mm_shuffle_ps(f, f, 0);

                __m128i x0i = _mm_loadu_si128((const __m128i*)src);
                __m128i x1i = _mm_srai_epi32(_mm_unpackhi_epi16(x0i, x0i), 16);
                x0i = _mm_srai_epi32(_mm_unpacklo_epi16(x0i, x0i), 16);
                x0 = _mm_cvtepi32_ps(x0i);
                x1 = _mm_cvtepi32_ps(x1i);
                s0 = _mm_add_ps(s0, _mm_mul_ps(x0, f));
                s1 = _mm_add_ps(s1, _mm_mul_ps(x1, f));
            }
            _mm_store_ps(dst + i, s0);
            _mm_store_ps(dst + i + 4, s1);
        }
        return i;
    }

    Mat kernel;
    bool sse2_supported;
};


struct SymmColumnVec_32f16s
{
    SymmColumnVec_32f16s() { symmetryType=0; }
    SymmColumnVec_32f16s(const Mat& _kernel, int _symmetryType, int, double _delta)
    {
        symmetryType = _symmetryType;
        kernel = _kernel;
        delta = (float)_delta;
        CV_Assert( (symmetryType & (KERNEL_SYMMETRICAL | KERNEL_ASYMMETRICAL)) != 0 );
        sse2_supported = checkHardwareSupport(CV_CPU_SSE2);
    }

    int operator()(const uchar** _src, uchar* _dst, int width) const
    {
        if( !sse2_supported )
            return 0;

        int ksize2 = (kernel.rows + kernel.cols - 1)/2;
        const float* ky = kernel.ptr<float>() + ksize2;
        int i = 0, k;
        bool symmetrical = (symmetryType & KERNEL_SYMMETRICAL) != 0;
        const float** src = (const float**)_src;
        const float *S, *S2;
        short* dst = (short*)_dst;
        __m128 d4 = _mm_set1_ps(delta);

        if( symmetrical )
        {
            for( ; i <= width - 16; i += 16 )
            {
                __m128 f = _mm_load_ss(ky);
                f = _mm_shuffle_ps(f, f, 0);
                __m128 s0, s1, s2, s3;
                __m128 x0, x1;
                S = src[0] + i;
                s0 = _mm_load_ps(S);
                s1 = _mm_load_ps(S+4);
                s0 = _mm_add_ps(_mm_mul_ps(s0, f), d4);
                s1 = _mm_add_ps(_mm_mul_ps(s1, f), d4);
                s2 = _mm_load_ps(S+8);
                s3 = _mm_load_ps(S+12);
                s2 = _mm_add_ps(_mm_mul_ps(s2, f), d4);
                s3 = _mm_add_ps(_mm_mul_ps(s3, f), d4);

                for( k = 1; k <= ksize2; k++ )
                {
                    S = src[k] + i;
                    S2 = src[-k] + i;
                    f = _mm_load_ss(ky+k);
                    f = _mm_shuffle_ps(f, f, 0);
                    x0 = _mm_add_ps(_mm_load_ps(S), _mm_load_ps(S2));
                    x1 = _mm_add_ps(_mm_load_ps(S+4), _mm_load_ps(S2+4));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(x0, f));
                    s1 = _mm_add_ps(s1, _mm_mul_ps(x1, f));
                    x0 = _mm_add_ps(_mm_load_ps(S+8), _mm_load_ps(S2+8));
                    x1 = _mm_add_ps(_mm_load_ps(S+12), _mm_load_ps(S2+12));
                    s2 = _mm_add_ps(s2, _mm_mul_ps(x0, f));
                    s3 = _mm_add_ps(s3, _mm_mul_ps(x1, f));
                }

                __m128i s0i = _mm_cvtps_epi32(s0);
                __m128i s1i = _mm_cvtps_epi32(s1);
                __m128i s2i = _mm_cvtps_epi32(s2);
                __m128i s3i = _mm_cvtps_epi32(s3);

                _mm_storeu_si128((__m128i*)(dst + i), _mm_packs_epi32(s0i, s1i));
                _mm_storeu_si128((__m128i*)(dst + i + 8), _mm_packs_epi32(s2i, s3i));
            }

            for( ; i <= width - 4; i += 4 )
            {
                __m128 f = _mm_load_ss(ky);
                f = _mm_shuffle_ps(f, f, 0);
                __m128 x0, s0 = _mm_load_ps(src[0] + i);
                s0 = _mm_add_ps(_mm_mul_ps(s0, f), d4);

                for( k = 1; k <= ksize2; k++ )
                {
                    f = _mm_load_ss(ky+k);
                    f = _mm_shuffle_ps(f, f, 0);
                    S = src[k] + i;
                    S2 = src[-k] + i;
                    x0 = _mm_add_ps(_mm_load_ps(src[k]+i), _mm_load_ps(src[-k] + i));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(x0, f));
                }

                __m128i s0i = _mm_cvtps_epi32(s0);
                _mm_storel_epi64((__m128i*)(dst + i), _mm_packs_epi32(s0i, s0i));
            }
        }
        else
        {
            for( ; i <= width - 16; i += 16 )
            {
                __m128 f, s0 = d4, s1 = d4, s2 = d4, s3 = d4;
                __m128 x0, x1;
                S = src[0] + i;

                for( k = 1; k <= ksize2; k++ )
                {
                    S = src[k] + i;
                    S2 = src[-k] + i;
                    f = _mm_load_ss(ky+k);
                    f = _mm_shuffle_ps(f, f, 0);
                    x0 = _mm_sub_ps(_mm_load_ps(S), _mm_load_ps(S2));
                    x1 = _mm_sub_ps(_mm_load_ps(S+4), _mm_load_ps(S2+4));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(x0, f));
                    s1 = _mm_add_ps(s1, _mm_mul_ps(x1, f));
                    x0 = _mm_sub_ps(_mm_load_ps(S+8), _mm_load_ps(S2+8));
                    x1 = _mm_sub_ps(_mm_load_ps(S+12), _mm_load_ps(S2+12));
                    s2 = _mm_add_ps(s2, _mm_mul_ps(x0, f));
                    s3 = _mm_add_ps(s3, _mm_mul_ps(x1, f));
                }

                __m128i s0i = _mm_cvtps_epi32(s0);
                __m128i s1i = _mm_cvtps_epi32(s1);
                __m128i s2i = _mm_cvtps_epi32(s2);
                __m128i s3i = _mm_cvtps_epi32(s3);

                _mm_storeu_si128((__m128i*)(dst + i), _mm_packs_epi32(s0i, s1i));
                _mm_storeu_si128((__m128i*)(dst + i + 8), _mm_packs_epi32(s2i, s3i));
            }

            for( ; i <= width - 4; i += 4 )
            {
                __m128 f, x0, s0 = d4;

                for( k = 1; k <= ksize2; k++ )
                {
                    f = _mm_load_ss(ky+k);
                    f = _mm_shuffle_ps(f, f, 0);
                    x0 = _mm_sub_ps(_mm_load_ps(src[k]+i), _mm_load_ps(src[-k] + i));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(x0, f));
                }

                __m128i s0i = _mm_cvtps_epi32(s0);
                _mm_storel_epi64((__m128i*)(dst + i), _mm_packs_epi32(s0i, s0i));
            }
        }

        return i;
    }

    int symmetryType;
    float delta;
    Mat kernel;
    bool sse2_supported;
};


/////////////////////////////////////// 32f //////////////////////////////////

struct RowVec_32f
{
    RowVec_32f()
    {
        haveSSE = checkHardwareSupport(CV_CPU_SSE);
    }

    RowVec_32f( const Mat& _kernel )
    {
        kernel = _kernel;
        haveSSE = checkHardwareSupport(CV_CPU_SSE);
#if defined USE_IPP_SEP_FILTERS && IPP_DISABLE_BLOCK
        bufsz = -1;
#endif
    }

    int operator()(const uchar* _src, uchar* _dst, int width, int cn) const
    {
#if defined USE_IPP_SEP_FILTERS && IPP_DISABLE_BLOCK
        CV_IPP_CHECK()
        {
            int ret = ippiOperator(_src, _dst, width, cn);
            if (ret > 0)
                return ret;
        }
#endif
        int _ksize = kernel.rows + kernel.cols - 1;
        const float* src0 = (const float*)_src;
        float* dst = (float*)_dst;
        const float* _kx = kernel.ptr<float>();

        if( !haveSSE )
            return 0;

        int i = 0, k;
        width *= cn;

        for( ; i <= width - 8; i += 8 )
        {
            const float* src = src0 + i;
            __m128 f, s0 = _mm_setzero_ps(), s1 = s0, x0, x1;
            for( k = 0; k < _ksize; k++, src += cn )
            {
                f = _mm_load_ss(_kx+k);
                f = _mm_shuffle_ps(f, f, 0);

                x0 = _mm_loadu_ps(src);
                x1 = _mm_loadu_ps(src + 4);
                s0 = _mm_add_ps(s0, _mm_mul_ps(x0, f));
                s1 = _mm_add_ps(s1, _mm_mul_ps(x1, f));
            }
            _mm_store_ps(dst + i, s0);
            _mm_store_ps(dst + i + 4, s1);
        }
        return i;
    }

    Mat kernel;
    bool haveSSE;
#if defined USE_IPP_SEP_FILTERS && IPP_DISABLE_BLOCK
private:
    mutable int bufsz;
    int ippiOperator(const uchar* _src, uchar* _dst, int width, int cn) const
    {
        CV_INSTRUMENT_REGION_IPP()

        int _ksize = kernel.rows + kernel.cols - 1;
        if ((1 != cn && 3 != cn) || width < _ksize*8)
            return 0;

        const float* src = (const float*)_src;
        float* dst = (float*)_dst;
        const float* _kx = (const float*)kernel.data;

        IppiSize roisz = { width, 1 };
        if( bufsz < 0 )
        {
            if( (cn == 1 && ippiFilterRowBorderPipelineGetBufferSize_32f_C1R(roisz, _ksize, &bufsz) < 0) ||
                (cn == 3 && ippiFilterRowBorderPipelineGetBufferSize_32f_C3R(roisz, _ksize, &bufsz) < 0))
                return 0;
        }
        AutoBuffer<uchar> buf(bufsz + 64);
        uchar* bufptr = alignPtr((uchar*)buf, 32);
        int step = (int)(width*sizeof(dst[0])*cn);
        float borderValue[] = {0.f, 0.f, 0.f};
        // here is the trick. IPP needs border type and extrapolates the row. We did it already.
        // So we pass anchor=0 and ignore the right tail of results since they are incorrect there.
        if( (cn == 1 && CV_INSTRUMENT_FUN_IPP(ippiFilterRowBorderPipeline_32f_C1R,(src, step, &dst, roisz, _kx, _ksize, 0,
                                                            ippBorderRepl, borderValue[0], bufptr)) < 0) ||
            (cn == 3 && CV_INSTRUMENT_FUN_IPP(ippiFilterRowBorderPipeline_32f_C3R,(src, step, &dst, roisz, _kx, _ksize, 0,
                                                            ippBorderRepl, borderValue, bufptr)) < 0))
        {
            setIppErrorStatus();
            return 0;
        }
        CV_IMPL_ADD(CV_IMPL_IPP);
        return width - _ksize + 1;
    }
#endif
};


struct SymmRowSmallVec_32f
{
    SymmRowSmallVec_32f() {}
    SymmRowSmallVec_32f( const Mat& _kernel, int _symmetryType )
    {
        kernel = _kernel;
        symmetryType = _symmetryType;
    }

    int operator()(const uchar* _src, uchar* _dst, int width, int cn) const
    {
        if( !checkHardwareSupport(CV_CPU_SSE) )
            return 0;

        int i = 0, _ksize = kernel.rows + kernel.cols - 1;
        float* dst = (float*)_dst;
        const float* src = (const float*)_src + (_ksize/2)*cn;
        bool symmetrical = (symmetryType & KERNEL_SYMMETRICAL) != 0;
        const float* kx = kernel.ptr<float>() + _ksize/2;
        width *= cn;

        if( symmetrical )
        {
            if( _ksize == 1 )
                return 0;
            if( _ksize == 3 )
            {
                if( kx[0] == 2 && kx[1] == 1 )
                    for( ; i <= width - 8; i += 8, src += 8 )
                    {
                        __m128 x0, x1, x2, y0, y1, y2;
                        x0 = _mm_loadu_ps(src - cn);
                        x1 = _mm_loadu_ps(src);
                        x2 = _mm_loadu_ps(src + cn);
                        y0 = _mm_loadu_ps(src - cn + 4);
                        y1 = _mm_loadu_ps(src + 4);
                        y2 = _mm_loadu_ps(src + cn + 4);
                        x0 = _mm_add_ps(x0, _mm_add_ps(_mm_add_ps(x1, x1), x2));
                        y0 = _mm_add_ps(y0, _mm_add_ps(_mm_add_ps(y1, y1), y2));
                        _mm_store_ps(dst + i, x0);
                        _mm_store_ps(dst + i + 4, y0);
                    }
                else if( kx[0] == -2 && kx[1] == 1 )
                    for( ; i <= width - 8; i += 8, src += 8 )
                    {
                        __m128 x0, x1, x2, y0, y1, y2;
                        x0 = _mm_loadu_ps(src - cn);
                        x1 = _mm_loadu_ps(src);
                        x2 = _mm_loadu_ps(src + cn);
                        y0 = _mm_loadu_ps(src - cn + 4);
                        y1 = _mm_loadu_ps(src + 4);
                        y2 = _mm_loadu_ps(src + cn + 4);
                        x0 = _mm_add_ps(x0, _mm_sub_ps(x2, _mm_add_ps(x1, x1)));
                        y0 = _mm_add_ps(y0, _mm_sub_ps(y2, _mm_add_ps(y1, y1)));
                        _mm_store_ps(dst + i, x0);
                        _mm_store_ps(dst + i + 4, y0);
                    }
                else
                {
                    __m128 k0 = _mm_set1_ps(kx[0]), k1 = _mm_set1_ps(kx[1]);
                    for( ; i <= width - 8; i += 8, src += 8 )
                    {
                        __m128 x0, x1, x2, y0, y1, y2;
                        x0 = _mm_loadu_ps(src - cn);
                        x1 = _mm_loadu_ps(src);
                        x2 = _mm_loadu_ps(src + cn);
                        y0 = _mm_loadu_ps(src - cn + 4);
                        y1 = _mm_loadu_ps(src + 4);
                        y2 = _mm_loadu_ps(src + cn + 4);

                        x0 = _mm_mul_ps(_mm_add_ps(x0, x2), k1);
                        y0 = _mm_mul_ps(_mm_add_ps(y0, y2), k1);
                        x0 = _mm_add_ps(x0, _mm_mul_ps(x1, k0));
                        y0 = _mm_add_ps(y0, _mm_mul_ps(y1, k0));
                        _mm_store_ps(dst + i, x0);
                        _mm_store_ps(dst + i + 4, y0);
                    }
                }
            }
            else if( _ksize == 5 )
            {
                if( kx[0] == -2 && kx[1] == 0 && kx[2] == 1 )
                    for( ; i <= width - 8; i += 8, src += 8 )
                    {
                        __m128 x0, x1, x2, y0, y1, y2;
                        x0 = _mm_loadu_ps(src - cn*2);
                        x1 = _mm_loadu_ps(src);
                        x2 = _mm_loadu_ps(src + cn*2);
                        y0 = _mm_loadu_ps(src - cn*2 + 4);
                        y1 = _mm_loadu_ps(src + 4);
                        y2 = _mm_loadu_ps(src + cn*2 + 4);
                        x0 = _mm_add_ps(x0, _mm_sub_ps(x2, _mm_add_ps(x1, x1)));
                        y0 = _mm_add_ps(y0, _mm_sub_ps(y2, _mm_add_ps(y1, y1)));
                        _mm_store_ps(dst + i, x0);
                        _mm_store_ps(dst + i + 4, y0);
                    }
                else
                {
                    __m128 k0 = _mm_set1_ps(kx[0]), k1 = _mm_set1_ps(kx[1]), k2 = _mm_set1_ps(kx[2]);
                    for( ; i <= width - 8; i += 8, src += 8 )
                    {
                        __m128 x0, x1, x2, y0, y1, y2;
                        x0 = _mm_loadu_ps(src - cn);
                        x1 = _mm_loadu_ps(src);
                        x2 = _mm_loadu_ps(src + cn);
                        y0 = _mm_loadu_ps(src - cn + 4);
                        y1 = _mm_loadu_ps(src + 4);
                        y2 = _mm_loadu_ps(src + cn + 4);

                        x0 = _mm_mul_ps(_mm_add_ps(x0, x2), k1);
                        y0 = _mm_mul_ps(_mm_add_ps(y0, y2), k1);
                        x0 = _mm_add_ps(x0, _mm_mul_ps(x1, k0));
                        y0 = _mm_add_ps(y0, _mm_mul_ps(y1, k0));

                        x2 = _mm_add_ps(_mm_loadu_ps(src + cn*2), _mm_loadu_ps(src - cn*2));
                        y2 = _mm_add_ps(_mm_loadu_ps(src + cn*2 + 4), _mm_loadu_ps(src - cn*2 + 4));
                        x0 = _mm_add_ps(x0, _mm_mul_ps(x2, k2));
                        y0 = _mm_add_ps(y0, _mm_mul_ps(y2, k2));

                        _mm_store_ps(dst + i, x0);
                        _mm_store_ps(dst + i + 4, y0);
                    }
                }
            }
        }
        else
        {
            if( _ksize == 3 )
            {
                if( kx[0] == 0 && kx[1] == 1 )
                    for( ; i <= width - 8; i += 8, src += 8 )
                    {
                        __m128 x0, x2, y0, y2;
                        x0 = _mm_loadu_ps(src + cn);
                        x2 = _mm_loadu_ps(src - cn);
                        y0 = _mm_loadu_ps(src + cn + 4);
                        y2 = _mm_loadu_ps(src - cn + 4);
                        x0 = _mm_sub_ps(x0, x2);
                        y0 = _mm_sub_ps(y0, y2);
                        _mm_store_ps(dst + i, x0);
                        _mm_store_ps(dst + i + 4, y0);
                    }
                else
                {
                    __m128 k1 = _mm_set1_ps(kx[1]);
                    for( ; i <= width - 8; i += 8, src += 8 )
                    {
                        __m128 x0, x2, y0, y2;
                        x0 = _mm_loadu_ps(src + cn);
                        x2 = _mm_loadu_ps(src - cn);
                        y0 = _mm_loadu_ps(src + cn + 4);
                        y2 = _mm_loadu_ps(src - cn + 4);

                        x0 = _mm_mul_ps(_mm_sub_ps(x0, x2), k1);
                        y0 = _mm_mul_ps(_mm_sub_ps(y0, y2), k1);
                        _mm_store_ps(dst + i, x0);
                        _mm_store_ps(dst + i + 4, y0);
                    }
                }
            }
            else if( _ksize == 5 )
            {
                __m128 k1 = _mm_set1_ps(kx[1]), k2 = _mm_set1_ps(kx[2]);
                for( ; i <= width - 8; i += 8, src += 8 )
                {
                    __m128 x0, x2, y0, y2;
                    x0 = _mm_loadu_ps(src + cn);
                    x2 = _mm_loadu_ps(src - cn);
                    y0 = _mm_loadu_ps(src + cn + 4);
                    y2 = _mm_loadu_ps(src - cn + 4);

                    x0 = _mm_mul_ps(_mm_sub_ps(x0, x2), k1);
                    y0 = _mm_mul_ps(_mm_sub_ps(y0, y2), k1);

                    x2 = _mm_sub_ps(_mm_loadu_ps(src + cn*2), _mm_loadu_ps(src - cn*2));
                    y2 = _mm_sub_ps(_mm_loadu_ps(src + cn*2 + 4), _mm_loadu_ps(src - cn*2 + 4));
                    x0 = _mm_add_ps(x0, _mm_mul_ps(x2, k2));
                    y0 = _mm_add_ps(y0, _mm_mul_ps(y2, k2));

                    _mm_store_ps(dst + i, x0);
                    _mm_store_ps(dst + i + 4, y0);
                }
            }
        }

        return i;
    }

    Mat kernel;
    int symmetryType;
};


struct SymmColumnVec_32f
{
    SymmColumnVec_32f() { symmetryType=0; }
    SymmColumnVec_32f(const Mat& _kernel, int _symmetryType, int, double _delta)
    {
        symmetryType = _symmetryType;
        kernel = _kernel;
        delta = (float)_delta;
        CV_Assert( (symmetryType & (KERNEL_SYMMETRICAL | KERNEL_ASYMMETRICAL)) != 0 );
    }

    int operator()(const uchar** _src, uchar* _dst, int width) const
    {
        if( !checkHardwareSupport(CV_CPU_SSE) )
            return 0;

        int ksize2 = (kernel.rows + kernel.cols - 1)/2;
        const float* ky = kernel.ptr<float>() + ksize2;
        int i = 0, k;
        bool symmetrical = (symmetryType & KERNEL_SYMMETRICAL) != 0;
        const float** src = (const float**)_src;
        const float *S, *S2;
        float* dst = (float*)_dst;
        __m128 d4 = _mm_set1_ps(delta);

        if( symmetrical )
        {
            for( ; i <= width - 16; i += 16 )
            {
                __m128 f = _mm_load_ss(ky);
                f = _mm_shuffle_ps(f, f, 0);
                __m128 s0, s1, s2, s3;
                __m128 x0, x1;
                S = src[0] + i;
                s0 = _mm_load_ps(S);
                s1 = _mm_load_ps(S+4);
                s0 = _mm_add_ps(_mm_mul_ps(s0, f), d4);
                s1 = _mm_add_ps(_mm_mul_ps(s1, f), d4);
                s2 = _mm_load_ps(S+8);
                s3 = _mm_load_ps(S+12);
                s2 = _mm_add_ps(_mm_mul_ps(s2, f), d4);
                s3 = _mm_add_ps(_mm_mul_ps(s3, f), d4);

                for( k = 1; k <= ksize2; k++ )
                {
                    S = src[k] + i;
                    S2 = src[-k] + i;
                    f = _mm_load_ss(ky+k);
                    f = _mm_shuffle_ps(f, f, 0);
                    x0 = _mm_add_ps(_mm_load_ps(S), _mm_load_ps(S2));
                    x1 = _mm_add_ps(_mm_load_ps(S+4), _mm_load_ps(S2+4));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(x0, f));
                    s1 = _mm_add_ps(s1, _mm_mul_ps(x1, f));
                    x0 = _mm_add_ps(_mm_load_ps(S+8), _mm_load_ps(S2+8));
                    x1 = _mm_add_ps(_mm_load_ps(S+12), _mm_load_ps(S2+12));
                    s2 = _mm_add_ps(s2, _mm_mul_ps(x0, f));
                    s3 = _mm_add_ps(s3, _mm_mul_ps(x1, f));
                }

                _mm_storeu_ps(dst + i, s0);
                _mm_storeu_ps(dst + i + 4, s1);
                _mm_storeu_ps(dst + i + 8, s2);
                _mm_storeu_ps(dst + i + 12, s3);
            }

            for( ; i <= width - 4; i += 4 )
            {
                __m128 f = _mm_load_ss(ky);
                f = _mm_shuffle_ps(f, f, 0);
                __m128 x0, s0 = _mm_load_ps(src[0] + i);
                s0 = _mm_add_ps(_mm_mul_ps(s0, f), d4);

                for( k = 1; k <= ksize2; k++ )
                {
                    f = _mm_load_ss(ky+k);
                    f = _mm_shuffle_ps(f, f, 0);
                    S = src[k] + i;
                    S2 = src[-k] + i;
                    x0 = _mm_add_ps(_mm_load_ps(src[k]+i), _mm_load_ps(src[-k] + i));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(x0, f));
                }

                _mm_storeu_ps(dst + i, s0);
            }
        }
        else
        {
            for( ; i <= width - 16; i += 16 )
            {
                __m128 f, s0 = d4, s1 = d4, s2 = d4, s3 = d4;
                __m128 x0, x1;
                S = src[0] + i;

                for( k = 1; k <= ksize2; k++ )
                {
                    S = src[k] + i;
                    S2 = src[-k] + i;
                    f = _mm_load_ss(ky+k);
                    f = _mm_shuffle_ps(f, f, 0);
                    x0 = _mm_sub_ps(_mm_load_ps(S), _mm_load_ps(S2));
                    x1 = _mm_sub_ps(_mm_load_ps(S+4), _mm_load_ps(S2+4));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(x0, f));
                    s1 = _mm_add_ps(s1, _mm_mul_ps(x1, f));
                    x0 = _mm_sub_ps(_mm_load_ps(S+8), _mm_load_ps(S2+8));
                    x1 = _mm_sub_ps(_mm_load_ps(S+12), _mm_load_ps(S2+12));
                    s2 = _mm_add_ps(s2, _mm_mul_ps(x0, f));
                    s3 = _mm_add_ps(s3, _mm_mul_ps(x1, f));
                }

                _mm_storeu_ps(dst + i, s0);
                _mm_storeu_ps(dst + i + 4, s1);
                _mm_storeu_ps(dst + i + 8, s2);
                _mm_storeu_ps(dst + i + 12, s3);
            }

            for( ; i <= width - 4; i += 4 )
            {
                __m128 f, x0, s0 = d4;

                for( k = 1; k <= ksize2; k++ )
                {
                    f = _mm_load_ss(ky+k);
                    f = _mm_shuffle_ps(f, f, 0);
                    x0 = _mm_sub_ps(_mm_load_ps(src[k]+i), _mm_load_ps(src[-k] + i));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(x0, f));
                }

                _mm_storeu_ps(dst + i, s0);
            }
        }

        return i;
    }

    int symmetryType;
    float delta;
    Mat kernel;
};


struct SymmColumnSmallVec_32f
{
    SymmColumnSmallVec_32f() { symmetryType=0; }
    SymmColumnSmallVec_32f(const Mat& _kernel, int _symmetryType, int, double _delta)
    {
        symmetryType = _symmetryType;
        kernel = _kernel;
        delta = (float)_delta;
        CV_Assert( (symmetryType & (KERNEL_SYMMETRICAL | KERNEL_ASYMMETRICAL)) != 0 );
    }

    int operator()(const uchar** _src, uchar* _dst, int width) const
    {
        if( !checkHardwareSupport(CV_CPU_SSE) )
            return 0;

        int ksize2 = (kernel.rows + kernel.cols - 1)/2;
        const float* ky = kernel.ptr<float>() + ksize2;
        int i = 0;
        bool symmetrical = (symmetryType & KERNEL_SYMMETRICAL) != 0;
        const float** src = (const float**)_src;
        const float *S0 = src[-1], *S1 = src[0], *S2 = src[1];
        float* dst = (float*)_dst;
        __m128 d4 = _mm_set1_ps(delta);

        if( symmetrical )
        {
            if( ky[0] == 2 && ky[1] == 1 )
            {
                for( ; i <= width - 8; i += 8 )
                {
                    __m128 s0, s1, s2, s3, s4, s5;
                    s0 = _mm_load_ps(S0 + i);
                    s1 = _mm_load_ps(S0 + i + 4);
                    s2 = _mm_load_ps(S1 + i);
                    s3 = _mm_load_ps(S1 + i + 4);
                    s4 = _mm_load_ps(S2 + i);
                    s5 = _mm_load_ps(S2 + i + 4);
                    s0 = _mm_add_ps(s0, _mm_add_ps(s4, _mm_add_ps(s2, s2)));
                    s1 = _mm_add_ps(s1, _mm_add_ps(s5, _mm_add_ps(s3, s3)));
                    s0 = _mm_add_ps(s0, d4);
                    s1 = _mm_add_ps(s1, d4);
                    _mm_storeu_ps(dst + i, s0);
                    _mm_storeu_ps(dst + i + 4, s1);
                }
            }
            else if( ky[0] == -2 && ky[1] == 1 )
            {
                for( ; i <= width - 8; i += 8 )
                {
                    __m128 s0, s1, s2, s3, s4, s5;
                    s0 = _mm_load_ps(S0 + i);
                    s1 = _mm_load_ps(S0 + i + 4);
                    s2 = _mm_load_ps(S1 + i);
                    s3 = _mm_load_ps(S1 + i + 4);
                    s4 = _mm_load_ps(S2 + i);
                    s5 = _mm_load_ps(S2 + i + 4);
                    s0 = _mm_add_ps(s0, _mm_sub_ps(s4, _mm_add_ps(s2, s2)));
                    s1 = _mm_add_ps(s1, _mm_sub_ps(s5, _mm_add_ps(s3, s3)));
                    s0 = _mm_add_ps(s0, d4);
                    s1 = _mm_add_ps(s1, d4);
                    _mm_storeu_ps(dst + i, s0);
                    _mm_storeu_ps(dst + i + 4, s1);
                }
            }
            else
            {
                __m128 k0 = _mm_set1_ps(ky[0]), k1 = _mm_set1_ps(ky[1]);
                for( ; i <= width - 8; i += 8 )
                {
                    __m128 s0, s1, x0, x1;
                    s0 = _mm_load_ps(S1 + i);
                    s1 = _mm_load_ps(S1 + i + 4);
                    s0 = _mm_add_ps(_mm_mul_ps(s0, k0), d4);
                    s1 = _mm_add_ps(_mm_mul_ps(s1, k0), d4);
                    x0 = _mm_add_ps(_mm_load_ps(S0 + i), _mm_load_ps(S2 + i));
                    x1 = _mm_add_ps(_mm_load_ps(S0 + i + 4), _mm_load_ps(S2 + i + 4));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(x0,k1));
                    s1 = _mm_add_ps(s1, _mm_mul_ps(x1,k1));
                    _mm_storeu_ps(dst + i, s0);
                    _mm_storeu_ps(dst + i + 4, s1);
                }
            }
        }
        else
        {
            if( fabs(ky[1]) == 1 && ky[1] == -ky[-1] )
            {
                if( ky[1] < 0 )
                    std::swap(S0, S2);
                for( ; i <= width - 8; i += 8 )
                {
                    __m128 s0, s1, s2, s3;
                    s0 = _mm_load_ps(S2 + i);
                    s1 = _mm_load_ps(S2 + i + 4);
                    s2 = _mm_load_ps(S0 + i);
                    s3 = _mm_load_ps(S0 + i + 4);
                    s0 = _mm_add_ps(_mm_sub_ps(s0, s2), d4);
                    s1 = _mm_add_ps(_mm_sub_ps(s1, s3), d4);
                    _mm_storeu_ps(dst + i, s0);
                    _mm_storeu_ps(dst + i + 4, s1);
                }
            }
            else
            {
                __m128 k1 = _mm_set1_ps(ky[1]);
                for( ; i <= width - 8; i += 8 )
                {
                    __m128 s0 = d4, s1 = d4, x0, x1;
                    x0 = _mm_sub_ps(_mm_load_ps(S2 + i), _mm_load_ps(S0 + i));
                    x1 = _mm_sub_ps(_mm_load_ps(S2 + i + 4), _mm_load_ps(S0 + i + 4));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(x0,k1));
                    s1 = _mm_add_ps(s1, _mm_mul_ps(x1,k1));
                    _mm_storeu_ps(dst + i, s0);
                    _mm_storeu_ps(dst + i + 4, s1);
                }
            }
        }

        return i;
    }

    int symmetryType;
    float delta;
    Mat kernel;
};


/////////////////////////////// non-separable filters ///////////////////////////////

///////////////////////////////// 8u<->8u, 8u<->16s /////////////////////////////////

struct FilterVec_8u
{
    FilterVec_8u() {}
    FilterVec_8u(const Mat& _kernel, int _bits, double _delta)
    {
        Mat kernel;
        _kernel.convertTo(kernel, CV_32F, 1./(1 << _bits), 0);
        delta = (float)(_delta/(1 << _bits));
        std::vector<Point> coords;
        preprocess2DKernel(kernel, coords, coeffs);
        _nz = (int)coords.size();
    }

    int operator()(const uchar** src, uchar* dst, int width) const
    {
        if( !checkHardwareSupport(CV_CPU_SSE2) )
            return 0;

        const float* kf = (const float*)&coeffs[0];
        int i = 0, k, nz = _nz;
        __m128 d4 = _mm_set1_ps(delta);

        for( ; i <= width - 16; i += 16 )
        {
            __m128 s0 = d4, s1 = d4, s2 = d4, s3 = d4;
            __m128i x0, x1, z = _mm_setzero_si128();

            for( k = 0; k < nz; k++ )
            {
                __m128 f = _mm_load_ss(kf+k), t0, t1;
                f = _mm_shuffle_ps(f, f, 0);

                x0 = _mm_loadu_si128((const __m128i*)(src[k] + i));
                x1 = _mm_unpackhi_epi8(x0, z);
                x0 = _mm_unpacklo_epi8(x0, z);

                t0 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(x0, z));
                t1 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(x0, z));
                s0 = _mm_add_ps(s0, _mm_mul_ps(t0, f));
                s1 = _mm_add_ps(s1, _mm_mul_ps(t1, f));

                t0 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(x1, z));
                t1 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(x1, z));
                s2 = _mm_add_ps(s2, _mm_mul_ps(t0, f));
                s3 = _mm_add_ps(s3, _mm_mul_ps(t1, f));
            }

            x0 = _mm_packs_epi32(_mm_cvtps_epi32(s0), _mm_cvtps_epi32(s1));
            x1 = _mm_packs_epi32(_mm_cvtps_epi32(s2), _mm_cvtps_epi32(s3));
            x0 = _mm_packus_epi16(x0, x1);
            _mm_storeu_si128((__m128i*)(dst + i), x0);
        }

        for( ; i <= width - 4; i += 4 )
        {
            __m128 s0 = d4;
            __m128i x0, z = _mm_setzero_si128();

            for( k = 0; k < nz; k++ )
            {
                __m128 f = _mm_load_ss(kf+k), t0;
                f = _mm_shuffle_ps(f, f, 0);

                x0 = _mm_cvtsi32_si128(*(const int*)(src[k] + i));
                x0 = _mm_unpacklo_epi8(x0, z);
                t0 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(x0, z));
                s0 = _mm_add_ps(s0, _mm_mul_ps(t0, f));
            }

            x0 = _mm_packs_epi32(_mm_cvtps_epi32(s0), z);
            x0 = _mm_packus_epi16(x0, x0);
            *(int*)(dst + i) = _mm_cvtsi128_si32(x0);
        }

        return i;
    }

    int _nz;
    std::vector<uchar> coeffs;
    float delta;
};


struct FilterVec_8u16s
{
    FilterVec_8u16s() {}
    FilterVec_8u16s(const Mat& _kernel, int _bits, double _delta)
    {
        Mat kernel;
        _kernel.convertTo(kernel, CV_32F, 1./(1 << _bits), 0);
        delta = (float)(_delta/(1 << _bits));
        std::vector<Point> coords;
        preprocess2DKernel(kernel, coords, coeffs);
        _nz = (int)coords.size();
    }

    int operator()(const uchar** src, uchar* _dst, int width) const
    {
        if( !checkHardwareSupport(CV_CPU_SSE2) )
            return 0;

        const float* kf = (const float*)&coeffs[0];
        short* dst = (short*)_dst;
        int i = 0, k, nz = _nz;
        __m128 d4 = _mm_set1_ps(delta);

        for( ; i <= width - 16; i += 16 )
        {
            __m128 s0 = d4, s1 = d4, s2 = d4, s3 = d4;
            __m128i x0, x1, z = _mm_setzero_si128();

            for( k = 0; k < nz; k++ )
            {
                __m128 f = _mm_load_ss(kf+k), t0, t1;
                f = _mm_shuffle_ps(f, f, 0);

                x0 = _mm_loadu_si128((const __m128i*)(src[k] + i));
                x1 = _mm_unpackhi_epi8(x0, z);
                x0 = _mm_unpacklo_epi8(x0, z);

                t0 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(x0, z));
                t1 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(x0, z));
                s0 = _mm_add_ps(s0, _mm_mul_ps(t0, f));
                s1 = _mm_add_ps(s1, _mm_mul_ps(t1, f));

                t0 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(x1, z));
                t1 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(x1, z));
                s2 = _mm_add_ps(s2, _mm_mul_ps(t0, f));
                s3 = _mm_add_ps(s3, _mm_mul_ps(t1, f));
            }

            x0 = _mm_packs_epi32(_mm_cvtps_epi32(s0), _mm_cvtps_epi32(s1));
            x1 = _mm_packs_epi32(_mm_cvtps_epi32(s2), _mm_cvtps_epi32(s3));
            _mm_storeu_si128((__m128i*)(dst + i), x0);
            _mm_storeu_si128((__m128i*)(dst + i + 8), x1);
        }

        for( ; i <= width - 4; i += 4 )
        {
            __m128 s0 = d4;
            __m128i x0, z = _mm_setzero_si128();

            for( k = 0; k < nz; k++ )
            {
                __m128 f = _mm_load_ss(kf+k), t0;
                f = _mm_shuffle_ps(f, f, 0);

                x0 = _mm_cvtsi32_si128(*(const int*)(src[k] + i));
                x0 = _mm_unpacklo_epi8(x0, z);
                t0 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(x0, z));
                s0 = _mm_add_ps(s0, _mm_mul_ps(t0, f));
            }

            x0 = _mm_packs_epi32(_mm_cvtps_epi32(s0), z);
            _mm_storel_epi64((__m128i*)(dst + i), x0);
        }

        return i;
    }

    int _nz;
    std::vector<uchar> coeffs;
    float delta;
};


struct FilterVec_32f
{
    FilterVec_32f() {}
    FilterVec_32f(const Mat& _kernel, int, double _delta)
    {
        delta = (float)_delta;
        std::vector<Point> coords;
        preprocess2DKernel(_kernel, coords, coeffs);
        _nz = (int)coords.size();
    }

    int operator()(const uchar** _src, uchar* _dst, int width) const
    {
        if( !checkHardwareSupport(CV_CPU_SSE) )
            return 0;

        const float* kf = (const float*)&coeffs[0];
        const float** src = (const float**)_src;
        float* dst = (float*)_dst;
        int i = 0, k, nz = _nz;
        __m128 d4 = _mm_set1_ps(delta);

        for( ; i <= width - 16; i += 16 )
        {
            __m128 s0 = d4, s1 = d4, s2 = d4, s3 = d4;

            for( k = 0; k < nz; k++ )
            {
                __m128 f = _mm_load_ss(kf+k), t0, t1;
                f = _mm_shuffle_ps(f, f, 0);
                const float* S = src[k] + i;

                t0 = _mm_loadu_ps(S);
                t1 = _mm_loadu_ps(S + 4);
                s0 = _mm_add_ps(s0, _mm_mul_ps(t0, f));
                s1 = _mm_add_ps(s1, _mm_mul_ps(t1, f));

                t0 = _mm_loadu_ps(S + 8);
                t1 = _mm_loadu_ps(S + 12);
                s2 = _mm_add_ps(s2, _mm_mul_ps(t0, f));
                s3 = _mm_add_ps(s3, _mm_mul_ps(t1, f));
            }

            _mm_storeu_ps(dst + i, s0);
            _mm_storeu_ps(dst + i + 4, s1);
            _mm_storeu_ps(dst + i + 8, s2);
            _mm_storeu_ps(dst + i + 12, s3);
        }

        for( ; i <= width - 4; i += 4 )
        {
            __m128 s0 = d4;

            for( k = 0; k < nz; k++ )
            {
                __m128 f = _mm_load_ss(kf+k), t0;
                f = _mm_shuffle_ps(f, f, 0);
                t0 = _mm_loadu_ps(src[k] + i);
                s0 = _mm_add_ps(s0, _mm_mul_ps(t0, f));
            }
            _mm_storeu_ps(dst + i, s0);
        }

        return i;
    }

    int _nz;
    std::vector<uchar> coeffs;
    float delta;
};


#elif CV_NEON

struct SymmRowSmallVec_8u32s
{
    SymmRowSmallVec_8u32s() { smallValues = false; }
    SymmRowSmallVec_8u32s( const Mat& _kernel, int _symmetryType )
    {
        kernel = _kernel;
        symmetryType = _symmetryType;
        smallValues = true;
        int k, ksize = kernel.rows + kernel.cols - 1;
        for( k = 0; k < ksize; k++ )
        {
            int v = kernel.ptr<int>()[k];
            if( v < SHRT_MIN || v > SHRT_MAX )
            {
                smallValues = false;
                break;
            }
        }
    }

    int operator()(const uchar* src, uchar* _dst, int width, int cn) const
    {
         if( !checkHardwareSupport(CV_CPU_NEON) )
             return 0;

        int i = 0, _ksize = kernel.rows + kernel.cols - 1;
        int* dst = (int*)_dst;
        bool symmetrical = (symmetryType & KERNEL_SYMMETRICAL) != 0;
        const int* kx = kernel.ptr<int>() + _ksize/2;
        if( !smallValues )
            return 0;

        src += (_ksize/2)*cn;
        width *= cn;

        if( symmetrical )
        {
            if( _ksize == 1 )
                return 0;
            if( _ksize == 3 )
            {
                if( kx[0] == 2 && kx[1] == 1 )
                {
                    uint16x8_t zq = vdupq_n_u16(0);

                    for( ; i <= width - 8; i += 8, src += 8 )
                    {
                        uint8x8_t x0, x1, x2;
                        x0 = vld1_u8( (uint8_t *) (src - cn) );
                        x1 = vld1_u8( (uint8_t *) (src) );
                        x2 = vld1_u8( (uint8_t *) (src + cn) );

                        uint16x8_t y0, y1, y2;
                        y0 = vaddl_u8(x0, x2);
                        y1 = vshll_n_u8(x1, 1);
                        y2 = vaddq_u16(y0, y1);

                        uint16x8x2_t str;
                        str.val[0] = y2; str.val[1] = zq;
                        vst2q_u16( (uint16_t *) (dst + i), str );
                    }
                }
                else if( kx[0] == -2 && kx[1] == 1 )
                    return 0;
                else
                {
                    int32x4_t k32 = vdupq_n_s32(0);
                    k32 = vld1q_lane_s32(kx, k32, 0);
                    k32 = vld1q_lane_s32(kx + 1, k32, 1);

                    int16x4_t k = vqmovn_s32(k32);

                    uint8x8_t z = vdup_n_u8(0);

                    for( ; i <= width - 8; i += 8, src += 8 )
                    {
                        uint8x8_t x0, x1, x2;
                        x0 = vld1_u8( (uint8_t *) (src - cn) );
                        x1 = vld1_u8( (uint8_t *) (src) );
                        x2 = vld1_u8( (uint8_t *) (src + cn) );

                        int16x8_t y0, y1;
                        int32x4_t y2, y3;
                        y0 = vreinterpretq_s16_u16(vaddl_u8(x1, z));
                        y1 = vreinterpretq_s16_u16(vaddl_u8(x0, x2));
                        y2 = vmull_lane_s16(vget_low_s16(y0), k, 0);
                        y2 = vmlal_lane_s16(y2, vget_low_s16(y1), k, 1);
                        y3 = vmull_lane_s16(vget_high_s16(y0), k, 0);
                        y3 = vmlal_lane_s16(y3, vget_high_s16(y1), k, 1);

                        vst1q_s32((int32_t *)(dst + i), y2);
                        vst1q_s32((int32_t *)(dst + i + 4), y3);
                    }
                }
            }
            else if( _ksize == 5 )
            {
                if( kx[0] == -2 && kx[1] == 0 && kx[2] == 1 )
                    return 0;
                else
                {
                    int32x4_t k32 = vdupq_n_s32(0);
                    k32 = vld1q_lane_s32(kx, k32, 0);
                    k32 = vld1q_lane_s32(kx + 1, k32, 1);
                    k32 = vld1q_lane_s32(kx + 2, k32, 2);

                    int16x4_t k = vqmovn_s32(k32);

                    uint8x8_t z = vdup_n_u8(0);

                    for( ; i <= width - 8; i += 8, src += 8 )
                    {
                        uint8x8_t x0, x1, x2, x3, x4;
                        x0 = vld1_u8( (uint8_t *) (src - cn) );
                        x1 = vld1_u8( (uint8_t *) (src) );
                        x2 = vld1_u8( (uint8_t *) (src + cn) );

                        int16x8_t y0, y1;
                        int32x4_t accl, acch;
                        y0 = vreinterpretq_s16_u16(vaddl_u8(x1, z));
                        y1 = vreinterpretq_s16_u16(vaddl_u8(x0, x2));
                        accl = vmull_lane_s16(vget_low_s16(y0), k, 0);
                        accl = vmlal_lane_s16(accl, vget_low_s16(y1), k, 1);
                        acch = vmull_lane_s16(vget_high_s16(y0), k, 0);
                        acch = vmlal_lane_s16(acch, vget_high_s16(y1), k, 1);

                        int16x8_t y2;
                        x3 = vld1_u8( (uint8_t *) (src - cn*2) );
                        x4 = vld1_u8( (uint8_t *) (src + cn*2) );
                        y2 = vreinterpretq_s16_u16(vaddl_u8(x3, x4));
                        accl = vmlal_lane_s16(accl, vget_low_s16(y2), k, 2);
                        acch = vmlal_lane_s16(acch, vget_high_s16(y2), k, 2);

                        vst1q_s32((int32_t *)(dst + i), accl);
                        vst1q_s32((int32_t *)(dst + i + 4), acch);
                    }
                }
            }
        }
        else
        {
            if( _ksize == 3 )
            {
                if( kx[0] == 0 && kx[1] == 1 )
                {
                    uint8x8_t z = vdup_n_u8(0);

                    for( ; i <= width - 8; i += 8, src += 8 )
                    {
                        uint8x8_t x0, x1;
                        x0 = vld1_u8( (uint8_t *) (src - cn) );
                        x1 = vld1_u8( (uint8_t *) (src + cn) );

                        int16x8_t y0;
                        y0 = vsubq_s16(vreinterpretq_s16_u16(vaddl_u8(x1, z)),
                                vreinterpretq_s16_u16(vaddl_u8(x0, z)));

                        vst1q_s32((int32_t *)(dst + i), vmovl_s16(vget_low_s16(y0)));
                        vst1q_s32((int32_t *)(dst + i + 4), vmovl_s16(vget_high_s16(y0)));
                    }
                }
                else
                {
                    int32x4_t k32 = vdupq_n_s32(0);
                    k32 = vld1q_lane_s32(kx + 1, k32, 1);

                    int16x4_t k = vqmovn_s32(k32);

                    uint8x8_t z = vdup_n_u8(0);

                    for( ; i <= width - 8; i += 8, src += 8 )
                    {
                        uint8x8_t x0, x1;
                        x0 = vld1_u8( (uint8_t *) (src - cn) );
                        x1 = vld1_u8( (uint8_t *) (src + cn) );

                        int16x8_t y0;
                        int32x4_t y1, y2;
                        y0 = vsubq_s16(vreinterpretq_s16_u16(vaddl_u8(x1, z)),
                            vreinterpretq_s16_u16(vaddl_u8(x0, z)));
                        y1 = vmull_lane_s16(vget_low_s16(y0), k, 1);
                        y2 = vmull_lane_s16(vget_high_s16(y0), k, 1);

                        vst1q_s32((int32_t *)(dst + i), y1);
                        vst1q_s32((int32_t *)(dst + i + 4), y2);
                    }
                }
            }
            else if( _ksize == 5 )
            {
                int32x4_t k32 = vdupq_n_s32(0);
                k32 = vld1q_lane_s32(kx + 1, k32, 1);
                k32 = vld1q_lane_s32(kx + 2, k32, 2);

                int16x4_t k = vqmovn_s32(k32);

                uint8x8_t z = vdup_n_u8(0);

                for( ; i <= width - 8; i += 8, src += 8 )
                {
                    uint8x8_t x0, x1;
                    x0 = vld1_u8( (uint8_t *) (src - cn) );
                    x1 = vld1_u8( (uint8_t *) (src + cn) );

                    int32x4_t accl, acch;
                    int16x8_t y0;
                    y0 = vsubq_s16(vreinterpretq_s16_u16(vaddl_u8(x1, z)),
                        vreinterpretq_s16_u16(vaddl_u8(x0, z)));
                    accl = vmull_lane_s16(vget_low_s16(y0), k, 1);
                    acch = vmull_lane_s16(vget_high_s16(y0), k, 1);

                    uint8x8_t x2, x3;
                    x2 = vld1_u8( (uint8_t *) (src - cn*2) );
                    x3 = vld1_u8( (uint8_t *) (src + cn*2) );

                    int16x8_t y1;
                    y1 = vsubq_s16(vreinterpretq_s16_u16(vaddl_u8(x3, z)),
                        vreinterpretq_s16_u16(vaddl_u8(x2, z)));
                    accl = vmlal_lane_s16(accl, vget_low_s16(y1), k, 2);
                    acch = vmlal_lane_s16(acch, vget_high_s16(y1), k, 2);

                    vst1q_s32((int32_t *)(dst + i), accl);
                    vst1q_s32((int32_t *)(dst + i + 4), acch);
                }
            }
        }

        return i;
    }

    Mat kernel;
    int symmetryType;
    bool smallValues;
};


struct SymmColumnVec_32s8u
{
    SymmColumnVec_32s8u() { symmetryType=0; }
    SymmColumnVec_32s8u(const Mat& _kernel, int _symmetryType, int _bits, double _delta)
    {
        symmetryType = _symmetryType;
        _kernel.convertTo(kernel, CV_32F, 1./(1 << _bits), 0);
        delta = (float)(_delta/(1 << _bits));
        CV_Assert( (symmetryType & (KERNEL_SYMMETRICAL | KERNEL_ASYMMETRICAL)) != 0 );
    }

    int operator()(const uchar** _src, uchar* dst, int width) const
    {
         if( !checkHardwareSupport(CV_CPU_NEON) )
             return 0;

        int _ksize = kernel.rows + kernel.cols - 1;
        int ksize2 = _ksize / 2;
        const float* ky = kernel.ptr<float>() + ksize2;
        int i = 0, k;
        bool symmetrical = (symmetryType & KERNEL_SYMMETRICAL) != 0;
        const int** src = (const int**)_src;
        const int *S, *S2;

        float32x4_t d4 = vdupq_n_f32(delta);

        if( symmetrical )
        {
            if( _ksize == 1 )
                return 0;


            float32x2_t k32;
            k32 = vdup_n_f32(0);
            k32 = vld1_lane_f32(ky, k32, 0);
            k32 = vld1_lane_f32(ky + 1, k32, 1);

            for( ; i <= width - 8; i += 8 )
            {
                float32x4_t accl, acch;
                float32x4_t f0l, f0h, f1l, f1h, f2l, f2h;

                S = src[0] + i;

                f0l = vcvtq_f32_s32( vld1q_s32(S) );
                f0h = vcvtq_f32_s32( vld1q_s32(S + 4) );

                S = src[1] + i;
                S2 = src[-1] + i;

                f1l = vcvtq_f32_s32( vld1q_s32(S) );
                f1h = vcvtq_f32_s32( vld1q_s32(S + 4) );
                f2l = vcvtq_f32_s32( vld1q_s32(S2) );
                f2h = vcvtq_f32_s32( vld1q_s32(S2 + 4) );

                accl = acch = d4;
                accl = vmlaq_lane_f32(accl, f0l, k32, 0);
                acch = vmlaq_lane_f32(acch, f0h, k32, 0);
                accl = vmlaq_lane_f32(accl, vaddq_f32(f1l, f2l), k32, 1);
                acch = vmlaq_lane_f32(acch, vaddq_f32(f1h, f2h), k32, 1);

                for( k = 2; k <= ksize2; k++ )
                {
                    S = src[k] + i;
                    S2 = src[-k] + i;

                    float32x4_t f3l, f3h, f4l, f4h;
                    f3l = vcvtq_f32_s32( vld1q_s32(S) );
                    f3h = vcvtq_f32_s32( vld1q_s32(S + 4) );
                    f4l = vcvtq_f32_s32( vld1q_s32(S2) );
                    f4h = vcvtq_f32_s32( vld1q_s32(S2 + 4) );

                    accl = vmlaq_n_f32(accl, vaddq_f32(f3l, f4l), ky[k]);
                    acch = vmlaq_n_f32(acch, vaddq_f32(f3h, f4h), ky[k]);
                }

                int32x4_t s32l, s32h;
                s32l = vcvtq_s32_f32(accl);
                s32h = vcvtq_s32_f32(acch);

                int16x4_t s16l, s16h;
                s16l = vqmovn_s32(s32l);
                s16h = vqmovn_s32(s32h);

                uint8x8_t u8;
                u8 =  vqmovun_s16(vcombine_s16(s16l, s16h));

                vst1_u8((uint8_t *)(dst + i), u8);
            }
        }
        else
        {
            float32x2_t k32;
            k32 = vdup_n_f32(0);
            k32 = vld1_lane_f32(ky + 1, k32, 1);

            for( ; i <= width - 8; i += 8 )
            {
                float32x4_t accl, acch;
                float32x4_t f1l, f1h, f2l, f2h;

                S = src[1] + i;
                S2 = src[-1] + i;

                f1l = vcvtq_f32_s32( vld1q_s32(S) );
                f1h = vcvtq_f32_s32( vld1q_s32(S + 4) );
                f2l = vcvtq_f32_s32( vld1q_s32(S2) );
                f2h = vcvtq_f32_s32( vld1q_s32(S2 + 4) );

                accl = acch = d4;
                accl = vmlaq_lane_f32(accl, vsubq_f32(f1l, f2l), k32, 1);
                acch = vmlaq_lane_f32(acch, vsubq_f32(f1h, f2h), k32, 1);

                for( k = 2; k <= ksize2; k++ )
                {
                    S = src[k] + i;
                    S2 = src[-k] + i;

                    float32x4_t f3l, f3h, f4l, f4h;
                    f3l = vcvtq_f32_s32( vld1q_s32(S) );
                    f3h = vcvtq_f32_s32( vld1q_s32(S + 4) );
                    f4l = vcvtq_f32_s32( vld1q_s32(S2) );
                    f4h = vcvtq_f32_s32( vld1q_s32(S2 + 4) );

                    accl = vmlaq_n_f32(accl, vsubq_f32(f3l, f4l), ky[k]);
                    acch = vmlaq_n_f32(acch, vsubq_f32(f3h, f4h), ky[k]);
                }

                int32x4_t s32l, s32h;
                s32l = vcvtq_s32_f32(accl);
                s32h = vcvtq_s32_f32(acch);

                int16x4_t s16l, s16h;
                s16l = vqmovn_s32(s32l);
                s16h = vqmovn_s32(s32h);

                uint8x8_t u8;
                u8 =  vqmovun_s16(vcombine_s16(s16l, s16h));

                vst1_u8((uint8_t *)(dst + i), u8);
            }
        }

        return i;
    }

    int symmetryType;
    float delta;
    Mat kernel;
};


struct SymmColumnSmallVec_32s16s
{
    SymmColumnSmallVec_32s16s() { symmetryType=0; }
    SymmColumnSmallVec_32s16s(const Mat& _kernel, int _symmetryType, int _bits, double _delta)
    {
        symmetryType = _symmetryType;
        _kernel.convertTo(kernel, CV_32F, 1./(1 << _bits), 0);
        delta = (float)(_delta/(1 << _bits));
        CV_Assert( (symmetryType & (KERNEL_SYMMETRICAL | KERNEL_ASYMMETRICAL)) != 0 );
    }

    int operator()(const uchar** _src, uchar* _dst, int width) const
    {
         if( !checkHardwareSupport(CV_CPU_NEON) )
             return 0;

        int ksize2 = (kernel.rows + kernel.cols - 1)/2;
        const float* ky = kernel.ptr<float>() + ksize2;
        int i = 0;
        bool symmetrical = (symmetryType & KERNEL_SYMMETRICAL) != 0;
        const int** src = (const int**)_src;
        const int *S0 = src[-1], *S1 = src[0], *S2 = src[1];
        short* dst = (short*)_dst;
        float32x4_t df4 = vdupq_n_f32(delta);
        int32x4_t d4 = vcvtq_s32_f32(df4);

        if( symmetrical )
        {
            if( ky[0] == 2 && ky[1] == 1 )
            {
                for( ; i <= width - 4; i += 4 )
                {
                    int32x4_t x0, x1, x2;
                    x0 = vld1q_s32((int32_t const *)(S0 + i));
                    x1 = vld1q_s32((int32_t const *)(S1 + i));
                    x2 = vld1q_s32((int32_t const *)(S2 + i));

                    int32x4_t y0, y1, y2, y3;
                    y0 = vaddq_s32(x0, x2);
                    y1 = vqshlq_n_s32(x1, 1);
                    y2 = vaddq_s32(y0, y1);
                    y3 = vaddq_s32(y2, d4);

                    int16x4_t t;
                    t = vqmovn_s32(y3);

                    vst1_s16((int16_t *)(dst + i), t);
                }
            }
            else if( ky[0] == -2 && ky[1] == 1 )
            {
                for( ; i <= width - 4; i += 4 )
                {
                    int32x4_t x0, x1, x2;
                    x0 = vld1q_s32((int32_t const *)(S0 + i));
                    x1 = vld1q_s32((int32_t const *)(S1 + i));
                    x2 = vld1q_s32((int32_t const *)(S2 + i));

                    int32x4_t y0, y1, y2, y3;
                    y0 = vaddq_s32(x0, x2);
                    y1 = vqshlq_n_s32(x1, 1);
                    y2 = vsubq_s32(y0, y1);
                    y3 = vaddq_s32(y2, d4);

                    int16x4_t t;
                    t = vqmovn_s32(y3);

                    vst1_s16((int16_t *)(dst + i), t);
                }
            }
            else if( ky[0] == 10 && ky[1] == 3 )
            {
                for( ; i <= width - 4; i += 4 )
                {
                    int32x4_t x0, x1, x2, x3;
                    x0 = vld1q_s32((int32_t const *)(S0 + i));
                    x1 = vld1q_s32((int32_t const *)(S1 + i));
                    x2 = vld1q_s32((int32_t const *)(S2 + i));

                    x3 = vaddq_s32(x0, x2);

                    int32x4_t y0;
                    y0 = vmlaq_n_s32(d4, x1, 10);
                    y0 = vmlaq_n_s32(y0, x3, 3);

                    int16x4_t t;
                    t = vqmovn_s32(y0);

                    vst1_s16((int16_t *)(dst + i), t);
                }
            }
            else
            {
                float32x2_t k32 = vdup_n_f32(0);
                k32 = vld1_lane_f32(ky, k32, 0);
                k32 = vld1_lane_f32(ky + 1, k32, 1);

                for( ; i <= width - 4; i += 4 )
                {
                    int32x4_t x0, x1, x2, x3, x4;
                    x0 = vld1q_s32((int32_t const *)(S0 + i));
                    x1 = vld1q_s32((int32_t const *)(S1 + i));
                    x2 = vld1q_s32((int32_t const *)(S2 + i));

                    x3 = vaddq_s32(x0, x2);

                    float32x4_t s0, s1, s2;
                    s0 = vcvtq_f32_s32(x1);
                    s1 = vcvtq_f32_s32(x3);
                    s2 = vmlaq_lane_f32(df4, s0, k32, 0);
                    s2 = vmlaq_lane_f32(s2, s1, k32, 1);

                    x4 = vcvtq_s32_f32(s2);

                    int16x4_t x5;
                    x5 = vqmovn_s32(x4);

                    vst1_s16((int16_t *)(dst + i), x5);
                }
            }
        }
        else
        {
            if( fabs(ky[1]) == 1 && ky[1] == -ky[-1] )
            {
                if( ky[1] < 0 )
                    std::swap(S0, S2);
                for( ; i <= width - 4; i += 4 )
                {
                    int32x4_t x0, x1;
                    x0 = vld1q_s32((int32_t const *)(S0 + i));
                    x1 = vld1q_s32((int32_t const *)(S2 + i));

                    int32x4_t y0, y1;
                    y0 = vsubq_s32(x1, x0);
                    y1 = vqaddq_s32(y0, d4);

                    int16x4_t t;
                    t = vqmovn_s32(y1);

                    vst1_s16((int16_t *)(dst + i), t);
                }
            }
            else
            {
                float32x2_t k32 = vdup_n_f32(0);
                k32 = vld1_lane_f32(ky + 1, k32, 1);

                for( ; i <= width - 4; i += 4 )
                {
                    int32x4_t x0, x1, x2, x3;
                    x0 = vld1q_s32((int32_t const *)(S0 + i));
                    x1 = vld1q_s32((int32_t const *)(S2 + i));

                    x2 = vsubq_s32(x1, x0);

                    float32x4_t s0, s1;
                    s0 = vcvtq_f32_s32(x2);
                    s1 = vmlaq_lane_f32(df4, s0, k32, 1);

                    x3 = vcvtq_s32_f32(s1);

                    int16x4_t x4;
                    x4 = vqmovn_s32(x3);

                    vst1_s16((int16_t *)(dst + i), x4);
                }
            }
        }

        return i;
    }

    int symmetryType;
    float delta;
    Mat kernel;
};


struct SymmColumnVec_32f16s
{
    SymmColumnVec_32f16s() { symmetryType=0; }
    SymmColumnVec_32f16s(const Mat& _kernel, int _symmetryType, int, double _delta)
    {
        symmetryType = _symmetryType;
        kernel = _kernel;
        delta = (float)_delta;
        CV_Assert( (symmetryType & (KERNEL_SYMMETRICAL | KERNEL_ASYMMETRICAL)) != 0 );
         neon_supported = checkHardwareSupport(CV_CPU_NEON);
    }

    int operator()(const uchar** _src, uchar* _dst, int width) const
    {
         if( !neon_supported )
             return 0;

        int _ksize = kernel.rows + kernel.cols - 1;
        int ksize2 = _ksize / 2;
        const float* ky = kernel.ptr<float>() + ksize2;
        int i = 0, k;
        bool symmetrical = (symmetryType & KERNEL_SYMMETRICAL) != 0;
        const float** src = (const float**)_src;
        const float *S, *S2;
        short* dst = (short*)_dst;

        float32x4_t d4 = vdupq_n_f32(delta);

        if( symmetrical )
        {
            if( _ksize == 1 )
                return 0;


            float32x2_t k32;
            k32 = vdup_n_f32(0);
            k32 = vld1_lane_f32(ky, k32, 0);
            k32 = vld1_lane_f32(ky + 1, k32, 1);

            for( ; i <= width - 8; i += 8 )
            {
                float32x4_t x0l, x0h, x1l, x1h, x2l, x2h;
                float32x4_t accl, acch;

                S = src[0] + i;

                x0l = vld1q_f32(S);
                x0h = vld1q_f32(S + 4);

                S = src[1] + i;
                S2 = src[-1] + i;

                x1l = vld1q_f32(S);
                x1h = vld1q_f32(S + 4);
                x2l = vld1q_f32(S2);
                x2h = vld1q_f32(S2 + 4);

                accl = acch = d4;
                accl = vmlaq_lane_f32(accl, x0l, k32, 0);
                acch = vmlaq_lane_f32(acch, x0h, k32, 0);
                accl = vmlaq_lane_f32(accl, vaddq_f32(x1l, x2l), k32, 1);
                acch = vmlaq_lane_f32(acch, vaddq_f32(x1h, x2h), k32, 1);

                for( k = 2; k <= ksize2; k++ )
                {
                    S = src[k] + i;
                    S2 = src[-k] + i;

                    float32x4_t x3l, x3h, x4l, x4h;
                    x3l = vld1q_f32(S);
                    x3h = vld1q_f32(S + 4);
                    x4l = vld1q_f32(S2);
                    x4h = vld1q_f32(S2 + 4);

                    accl = vmlaq_n_f32(accl, vaddq_f32(x3l, x4l), ky[k]);
                    acch = vmlaq_n_f32(acch, vaddq_f32(x3h, x4h), ky[k]);
                }

                int32x4_t s32l, s32h;
                s32l = vcvtq_s32_f32(accl);
                s32h = vcvtq_s32_f32(acch);

                int16x4_t s16l, s16h;
                s16l = vqmovn_s32(s32l);
                s16h = vqmovn_s32(s32h);

                vst1_s16((int16_t *)(dst + i), s16l);
                vst1_s16((int16_t *)(dst + i + 4), s16h);
            }
        }
        else
        {
            float32x2_t k32;
            k32 = vdup_n_f32(0);
            k32 = vld1_lane_f32(ky + 1, k32, 1);

            for( ; i <= width - 8; i += 8 )
            {
                float32x4_t x1l, x1h, x2l, x2h;
                float32x4_t accl, acch;

                S = src[1] + i;
                S2 = src[-1] + i;

                x1l = vld1q_f32(S);
                x1h = vld1q_f32(S + 4);
                x2l = vld1q_f32(S2);
                x2h = vld1q_f32(S2 + 4);

                accl = acch = d4;
                accl = vmlaq_lane_f32(accl, vsubq_f32(x1l, x2l), k32, 1);
                acch = vmlaq_lane_f32(acch, vsubq_f32(x1h, x2h), k32, 1);

                for( k = 2; k <= ksize2; k++ )
                {
                    S = src[k] + i;
                    S2 = src[-k] + i;

                    float32x4_t x3l, x3h, x4l, x4h;
                    x3l = vld1q_f32(S);
                    x3h = vld1q_f32(S + 4);
                    x4l = vld1q_f32(S2);
                    x4h = vld1q_f32(S2 + 4);

                    accl = vmlaq_n_f32(accl, vsubq_f32(x3l, x4l), ky[k]);
                    acch = vmlaq_n_f32(acch, vsubq_f32(x3h, x4h), ky[k]);
                }

                int32x4_t s32l, s32h;
                s32l = vcvtq_s32_f32(accl);
                s32h = vcvtq_s32_f32(acch);

                int16x4_t s16l, s16h;
                s16l = vqmovn_s32(s32l);
                s16h = vqmovn_s32(s32h);

                vst1_s16((int16_t *)(dst + i), s16l);
                vst1_s16((int16_t *)(dst + i + 4), s16h);
            }
        }

        return i;
    }

    int symmetryType;
    float delta;
    Mat kernel;
    bool neon_supported;
};


struct SymmRowSmallVec_32f
{
    SymmRowSmallVec_32f() {}
    SymmRowSmallVec_32f( const Mat& _kernel, int _symmetryType )
    {
        kernel = _kernel;
        symmetryType = _symmetryType;
    }

    int operator()(const uchar* _src, uchar* _dst, int width, int cn) const
    {
         if( !checkHardwareSupport(CV_CPU_NEON) )
             return 0;

        int i = 0, _ksize = kernel.rows + kernel.cols - 1;
        float* dst = (float*)_dst;
        const float* src = (const float*)_src + (_ksize/2)*cn;
        bool symmetrical = (symmetryType & KERNEL_SYMMETRICAL) != 0;
        const float* kx = kernel.ptr<float>() + _ksize/2;
        width *= cn;

        if( symmetrical )
        {
            if( _ksize == 1 )
                return 0;
            if( _ksize == 3 )
            {
                if( kx[0] == 2 && kx[1] == 1 )
                    return 0;
                else if( kx[0] == -2 && kx[1] == 1 )
                    return 0;
                else
                {
                    return 0;
                }
            }
            else if( _ksize == 5 )
            {
                if( kx[0] == -2 && kx[1] == 0 && kx[2] == 1 )
                    return 0;
                else
                {
                    float32x2_t k0, k1;
                    k0 = k1 = vdup_n_f32(0);
                    k0 = vld1_lane_f32(kx + 0, k0, 0);
                    k0 = vld1_lane_f32(kx + 1, k0, 1);
                    k1 = vld1_lane_f32(kx + 2, k1, 0);

                    for( ; i <= width - 4; i += 4, src += 4 )
                    {
                        float32x4_t x0, x1, x2, x3, x4;
                        x0 = vld1q_f32(src);
                        x1 = vld1q_f32(src - cn);
                        x2 = vld1q_f32(src + cn);
                        x3 = vld1q_f32(src - cn*2);
                        x4 = vld1q_f32(src + cn*2);

                        float32x4_t y0;
                        y0 = vmulq_lane_f32(x0, k0, 0);
                        y0 = vmlaq_lane_f32(y0, vaddq_f32(x1, x2), k0, 1);
                        y0 = vmlaq_lane_f32(y0, vaddq_f32(x3, x4), k1, 0);

                        vst1q_f32(dst + i, y0);
                    }
                }
            }
        }
        else
        {
            if( _ksize == 3 )
            {
                if( kx[0] == 0 && kx[1] == 1 )
                    return 0;
                else
                {
                    return 0;
                }
            }
            else if( _ksize == 5 )
            {
                float32x2_t k;
                k = vdup_n_f32(0);
                k = vld1_lane_f32(kx + 1, k, 0);
                k = vld1_lane_f32(kx + 2, k, 1);

                for( ; i <= width - 4; i += 4, src += 4 )
                {
                    float32x4_t x0, x1, x2, x3;
                    x0 = vld1q_f32(src - cn);
                    x1 = vld1q_f32(src + cn);
                    x2 = vld1q_f32(src - cn*2);
                    x3 = vld1q_f32(src + cn*2);

                    float32x4_t y0;
                    y0 = vmulq_lane_f32(vsubq_f32(x1, x0), k, 0);
                    y0 = vmlaq_lane_f32(y0, vsubq_f32(x3, x2), k, 1);

                    vst1q_f32(dst + i, y0);
                }
            }
        }

        return i;
    }

    Mat kernel;
    int symmetryType;
};


typedef RowNoVec RowVec_8u32s;
typedef RowNoVec RowVec_16s32f;
typedef RowNoVec RowVec_32f;
typedef ColumnNoVec SymmColumnVec_32f;
typedef SymmColumnSmallNoVec SymmColumnSmallVec_32f;
typedef FilterNoVec FilterVec_8u;
typedef FilterNoVec FilterVec_8u16s;
typedef FilterNoVec FilterVec_32f;


#else

typedef RowNoVec RowVec_8u32s;
typedef RowNoVec RowVec_16s32f;
typedef RowNoVec RowVec_32f;
typedef SymmRowSmallNoVec SymmRowSmallVec_8u32s;
typedef SymmRowSmallNoVec SymmRowSmallVec_32f;
typedef ColumnNoVec SymmColumnVec_32s8u;
typedef ColumnNoVec SymmColumnVec_32f16s;
typedef ColumnNoVec SymmColumnVec_32f;
typedef SymmColumnSmallNoVec SymmColumnSmallVec_32s16s;
typedef SymmColumnSmallNoVec SymmColumnSmallVec_32f;
typedef FilterNoVec FilterVec_8u;
typedef FilterNoVec FilterVec_8u16s;
typedef FilterNoVec FilterVec_32f;

#endif


template<typename ST, typename DT, class VecOp> struct RowFilter : public BaseRowFilter
{
    RowFilter( const Mat& _kernel, int _anchor, const VecOp& _vecOp=VecOp() )
    {
        if( _kernel.isContinuous() )
            kernel = _kernel;
        else
            _kernel.copyTo(kernel);
        anchor = _anchor;
        ksize = kernel.rows + kernel.cols - 1;
        CV_Assert( kernel.type() == DataType<DT>::type &&
                   (kernel.rows == 1 || kernel.cols == 1));
        vecOp = _vecOp;
    }

    void operator()(const uchar* src, uchar* dst, int width, int cn)
    {
        int _ksize = ksize;
        const DT* kx = kernel.ptr<DT>();
        const ST* S;
        DT* D = (DT*)dst;
        int i, k;

        i = vecOp(src, dst, width, cn);
        width *= cn;
        #if CV_ENABLE_UNROLLED
        for( ; i <= width - 4; i += 4 )
        {
            S = (const ST*)src + i;
            DT f = kx[0];
            DT s0 = f*S[0], s1 = f*S[1], s2 = f*S[2], s3 = f*S[3];

            for( k = 1; k < _ksize; k++ )
            {
                S += cn;
                f = kx[k];
                s0 += f*S[0]; s1 += f*S[1];
                s2 += f*S[2]; s3 += f*S[3];
            }

            D[i] = s0; D[i+1] = s1;
            D[i+2] = s2; D[i+3] = s3;
        }
        #endif
        for( ; i < width; i++ )
        {
            S = (const ST*)src + i;
            DT s0 = kx[0]*S[0];
            for( k = 1; k < _ksize; k++ )
            {
                S += cn;
                s0 += kx[k]*S[0];
            }
            D[i] = s0;
        }
    }

    Mat kernel;
    VecOp vecOp;
};


template<typename ST, typename DT, class VecOp> struct SymmRowSmallFilter :
    public RowFilter<ST, DT, VecOp>
{
    SymmRowSmallFilter( const Mat& _kernel, int _anchor, int _symmetryType,
                        const VecOp& _vecOp = VecOp())
        : RowFilter<ST, DT, VecOp>( _kernel, _anchor, _vecOp )
    {
        symmetryType = _symmetryType;
        CV_Assert( (symmetryType & (KERNEL_SYMMETRICAL | KERNEL_ASYMMETRICAL)) != 0 && this->ksize <= 5 );
    }

    void operator()(const uchar* src, uchar* dst, int width, int cn)
    {
        int ksize2 = this->ksize/2, ksize2n = ksize2*cn;
        const DT* kx = this->kernel.template ptr<DT>() + ksize2;
        bool symmetrical = (this->symmetryType & KERNEL_SYMMETRICAL) != 0;
        DT* D = (DT*)dst;
        int i = this->vecOp(src, dst, width, cn), j, k;
        const ST* S = (const ST*)src + i + ksize2n;
        width *= cn;

        if( symmetrical )
        {
            if( this->ksize == 1 && kx[0] == 1 )
            {
                for( ; i <= width - 2; i += 2 )
                {
                    DT s0 = S[i], s1 = S[i+1];
                    D[i] = s0; D[i+1] = s1;
                }
                S += i;
            }
            else if( this->ksize == 3 )
            {
                if( kx[0] == 2 && kx[1] == 1 )
                    for( ; i <= width - 2; i += 2, S += 2 )
                    {
                        DT s0 = S[-cn] + S[0]*2 + S[cn], s1 = S[1-cn] + S[1]*2 + S[1+cn];
                        D[i] = s0; D[i+1] = s1;
                    }
                else if( kx[0] == -2 && kx[1] == 1 )
                    for( ; i <= width - 2; i += 2, S += 2 )
                    {
                        DT s0 = S[-cn] - S[0]*2 + S[cn], s1 = S[1-cn] - S[1]*2 + S[1+cn];
                        D[i] = s0; D[i+1] = s1;
                    }
                else
                {
                    DT k0 = kx[0], k1 = kx[1];
                    for( ; i <= width - 2; i += 2, S += 2 )
                    {
                        DT s0 = S[0]*k0 + (S[-cn] + S[cn])*k1, s1 = S[1]*k0 + (S[1-cn] + S[1+cn])*k1;
                        D[i] = s0; D[i+1] = s1;
                    }
                }
            }
            else if( this->ksize == 5 )
            {
                DT k0 = kx[0], k1 = kx[1], k2 = kx[2];
                if( k0 == -2 && k1 == 0 && k2 == 1 )
                    for( ; i <= width - 2; i += 2, S += 2 )
                    {
                        DT s0 = -2*S[0] + S[-cn*2] + S[cn*2];
                        DT s1 = -2*S[1] + S[1-cn*2] + S[1+cn*2];
                        D[i] = s0; D[i+1] = s1;
                    }
                else
                    for( ; i <= width - 2; i += 2, S += 2 )
                    {
                        DT s0 = S[0]*k0 + (S[-cn] + S[cn])*k1 + (S[-cn*2] + S[cn*2])*k2;
                        DT s1 = S[1]*k0 + (S[1-cn] + S[1+cn])*k1 + (S[1-cn*2] + S[1+cn*2])*k2;
                        D[i] = s0; D[i+1] = s1;
                    }
            }

            for( ; i < width; i++, S++ )
            {
                DT s0 = kx[0]*S[0];
                for( k = 1, j = cn; k <= ksize2; k++, j += cn )
                    s0 += kx[k]*(S[j] + S[-j]);
                D[i] = s0;
            }
        }
        else
        {
            if( this->ksize == 3 )
            {
                if( kx[0] == 0 && kx[1] == 1 )
                    for( ; i <= width - 2; i += 2, S += 2 )
                    {
                        DT s0 = S[cn] - S[-cn], s1 = S[1+cn] - S[1-cn];
                        D[i] = s0; D[i+1] = s1;
                    }
                else
                {
                    DT k1 = kx[1];
                    for( ; i <= width - 2; i += 2, S += 2 )
                    {
                        DT s0 = (S[cn] - S[-cn])*k1, s1 = (S[1+cn] - S[1-cn])*k1;
                        D[i] = s0; D[i+1] = s1;
                    }
                }
            }
            else if( this->ksize == 5 )
            {
                DT k1 = kx[1], k2 = kx[2];
                for( ; i <= width - 2; i += 2, S += 2 )
                {
                    DT s0 = (S[cn] - S[-cn])*k1 + (S[cn*2] - S[-cn*2])*k2;
                    DT s1 = (S[1+cn] - S[1-cn])*k1 + (S[1+cn*2] - S[1-cn*2])*k2;
                    D[i] = s0; D[i+1] = s1;
                }
            }

            for( ; i < width; i++, S++ )
            {
                DT s0 = kx[0]*S[0];
                for( k = 1, j = cn; k <= ksize2; k++, j += cn )
                    s0 += kx[k]*(S[j] - S[-j]);
                D[i] = s0;
            }
        }
    }

    int symmetryType;
};


template<class CastOp, class VecOp> struct ColumnFilter : public BaseColumnFilter
{
    typedef typename CastOp::type1 ST;
    typedef typename CastOp::rtype DT;

    ColumnFilter( const Mat& _kernel, int _anchor,
        double _delta, const CastOp& _castOp=CastOp(),
        const VecOp& _vecOp=VecOp() )
    {
        if( _kernel.isContinuous() )
            kernel = _kernel;
        else
            _kernel.copyTo(kernel);
        anchor = _anchor;
        ksize = kernel.rows + kernel.cols - 1;
        delta = saturate_cast<ST>(_delta);
        castOp0 = _castOp;
        vecOp = _vecOp;
        CV_Assert( kernel.type() == DataType<ST>::type &&
                   (kernel.rows == 1 || kernel.cols == 1));
    }

    void operator()(const uchar** src, uchar* dst, int dststep, int count, int width)
    {
        const ST* ky = kernel.template ptr<ST>();
        ST _delta = delta;
        int _ksize = ksize;
        int i, k;
        CastOp castOp = castOp0;

        for( ; count--; dst += dststep, src++ )
        {
            DT* D = (DT*)dst;
            i = vecOp(src, dst, width);
            #if CV_ENABLE_UNROLLED
            for( ; i <= width - 4; i += 4 )
            {
                ST f = ky[0];
                const ST* S = (const ST*)src[0] + i;
                ST s0 = f*S[0] + _delta, s1 = f*S[1] + _delta,
                    s2 = f*S[2] + _delta, s3 = f*S[3] + _delta;

                for( k = 1; k < _ksize; k++ )
                {
                    S = (const ST*)src[k] + i; f = ky[k];
                    s0 += f*S[0]; s1 += f*S[1];
                    s2 += f*S[2]; s3 += f*S[3];
                }

                D[i] = castOp(s0); D[i+1] = castOp(s1);
                D[i+2] = castOp(s2); D[i+3] = castOp(s3);
            }
            #endif
            for( ; i < width; i++ )
            {
                ST s0 = ky[0]*((const ST*)src[0])[i] + _delta;
                for( k = 1; k < _ksize; k++ )
                    s0 += ky[k]*((const ST*)src[k])[i];
                D[i] = castOp(s0);
            }
        }
    }

    Mat kernel;
    CastOp castOp0;
    VecOp vecOp;
    ST delta;
};


template<class CastOp, class VecOp> struct SymmColumnFilter : public ColumnFilter<CastOp, VecOp>
{
    typedef typename CastOp::type1 ST;
    typedef typename CastOp::rtype DT;

    SymmColumnFilter( const Mat& _kernel, int _anchor,
        double _delta, int _symmetryType,
        const CastOp& _castOp=CastOp(),
        const VecOp& _vecOp=VecOp())
        : ColumnFilter<CastOp, VecOp>( _kernel, _anchor, _delta, _castOp, _vecOp )
    {
        symmetryType = _symmetryType;
        CV_Assert( (symmetryType & (KERNEL_SYMMETRICAL | KERNEL_ASYMMETRICAL)) != 0 );
    }

    void operator()(const uchar** src, uchar* dst, int dststep, int count, int width)
    {
        int ksize2 = this->ksize/2;
        const ST* ky = this->kernel.template ptr<ST>() + ksize2;
        int i, k;
        bool symmetrical = (symmetryType & KERNEL_SYMMETRICAL) != 0;
        ST _delta = this->delta;
        CastOp castOp = this->castOp0;
        src += ksize2;

        if( symmetrical )
        {
            for( ; count--; dst += dststep, src++ )
            {
                DT* D = (DT*)dst;
                i = (this->vecOp)(src, dst, width);
                #if CV_ENABLE_UNROLLED
                for( ; i <= width - 4; i += 4 )
                {
                    ST f = ky[0];
                    const ST* S = (const ST*)src[0] + i, *S2;
                    ST s0 = f*S[0] + _delta, s1 = f*S[1] + _delta,
                        s2 = f*S[2] + _delta, s3 = f*S[3] + _delta;

                    for( k = 1; k <= ksize2; k++ )
                    {
                        S = (const ST*)src[k] + i;
                        S2 = (const ST*)src[-k] + i;
                        f = ky[k];
                        s0 += f*(S[0] + S2[0]);
                        s1 += f*(S[1] + S2[1]);
                        s2 += f*(S[2] + S2[2]);
                        s3 += f*(S[3] + S2[3]);
                    }

                    D[i] = castOp(s0); D[i+1] = castOp(s1);
                    D[i+2] = castOp(s2); D[i+3] = castOp(s3);
                }
                #endif
                for( ; i < width; i++ )
                {
                    ST s0 = ky[0]*((const ST*)src[0])[i] + _delta;
                    for( k = 1; k <= ksize2; k++ )
                        s0 += ky[k]*(((const ST*)src[k])[i] + ((const ST*)src[-k])[i]);
                    D[i] = castOp(s0);
                }
            }
        }
        else
        {
            for( ; count--; dst += dststep, src++ )
            {
                DT* D = (DT*)dst;
                i = this->vecOp(src, dst, width);
                #if CV_ENABLE_UNROLLED
                for( ; i <= width - 4; i += 4 )
                {
                    ST f = ky[0];
                    const ST *S, *S2;
                    ST s0 = _delta, s1 = _delta, s2 = _delta, s3 = _delta;

                    for( k = 1; k <= ksize2; k++ )
                    {
                        S = (const ST*)src[k] + i;
                        S2 = (const ST*)src[-k] + i;
                        f = ky[k];
                        s0 += f*(S[0] - S2[0]);
                        s1 += f*(S[1] - S2[1]);
                        s2 += f*(S[2] - S2[2]);
                        s3 += f*(S[3] - S2[3]);
                    }

                    D[i] = castOp(s0); D[i+1] = castOp(s1);
                    D[i+2] = castOp(s2); D[i+3] = castOp(s3);
                }
                #endif
                for( ; i < width; i++ )
                {
                    ST s0 = _delta;
                    for( k = 1; k <= ksize2; k++ )
                        s0 += ky[k]*(((const ST*)src[k])[i] - ((const ST*)src[-k])[i]);
                    D[i] = castOp(s0);
                }
            }
        }
    }

    int symmetryType;
};


template<class CastOp, class VecOp>
struct SymmColumnSmallFilter : public SymmColumnFilter<CastOp, VecOp>
{
    typedef typename CastOp::type1 ST;
    typedef typename CastOp::rtype DT;

    SymmColumnSmallFilter( const Mat& _kernel, int _anchor,
                           double _delta, int _symmetryType,
                           const CastOp& _castOp=CastOp(),
                           const VecOp& _vecOp=VecOp())
        : SymmColumnFilter<CastOp, VecOp>( _kernel, _anchor, _delta, _symmetryType, _castOp, _vecOp )
    {
        CV_Assert( this->ksize == 3 );
    }

    void operator()(const uchar** src, uchar* dst, int dststep, int count, int width)
    {
        int ksize2 = this->ksize/2;
        const ST* ky = this->kernel.template ptr<ST>() + ksize2;
        int i;
        bool symmetrical = (this->symmetryType & KERNEL_SYMMETRICAL) != 0;
        bool is_1_2_1 = ky[0] == 2 && ky[1] == 1;
        bool is_1_m2_1 = ky[0] == -2 && ky[1] == 1;
        bool is_m1_0_1 = ky[0] == 0 && (ky[1] == 1 || ky[1] == -1);
        ST f0 = ky[0], f1 = ky[1];
        ST _delta = this->delta;
        CastOp castOp = this->castOp0;
        src += ksize2;

        for( ; count--; dst += dststep, src++ )
        {
            DT* D = (DT*)dst;
            i = (this->vecOp)(src, dst, width);
            const ST* S0 = (const ST*)src[-1];
            const ST* S1 = (const ST*)src[0];
            const ST* S2 = (const ST*)src[1];

            if( symmetrical )
            {
                if( is_1_2_1 )
                {
                    #if CV_ENABLE_UNROLLED
                    for( ; i <= width - 4; i += 4 )
                    {
                        ST s0 = S0[i] + S1[i]*2 + S2[i] + _delta;
                        ST s1 = S0[i+1] + S1[i+1]*2 + S2[i+1] + _delta;
                        D[i] = castOp(s0);
                        D[i+1] = castOp(s1);

                        s0 = S0[i+2] + S1[i+2]*2 + S2[i+2] + _delta;
                        s1 = S0[i+3] + S1[i+3]*2 + S2[i+3] + _delta;
                        D[i+2] = castOp(s0);
                        D[i+3] = castOp(s1);
                    }
                    #endif
                    for( ; i < width; i ++ )
                    {
                        ST s0 = S0[i] + S1[i]*2 + S2[i] + _delta;
                        D[i] = castOp(s0);
                    }
                }
                else if( is_1_m2_1 )
                {
                    #if CV_ENABLE_UNROLLED
                    for( ; i <= width - 4; i += 4 )
                    {
                        ST s0 = S0[i] - S1[i]*2 + S2[i] + _delta;
                        ST s1 = S0[i+1] - S1[i+1]*2 + S2[i+1] + _delta;
                        D[i] = castOp(s0);
                        D[i+1] = castOp(s1);

                        s0 = S0[i+2] - S1[i+2]*2 + S2[i+2] + _delta;
                        s1 = S0[i+3] - S1[i+3]*2 + S2[i+3] + _delta;
                        D[i+2] = castOp(s0);
                        D[i+3] = castOp(s1);
                    }
                    #endif
                    for( ; i < width; i ++ )
                    {
                        ST s0 = S0[i] - S1[i]*2 + S2[i] + _delta;
                        D[i] = castOp(s0);
                    }
                }
                else
                {
                    #if CV_ENABLE_UNROLLED
                    for( ; i <= width - 4; i += 4 )
                    {
                        ST s0 = (S0[i] + S2[i])*f1 + S1[i]*f0 + _delta;
                        ST s1 = (S0[i+1] + S2[i+1])*f1 + S1[i+1]*f0 + _delta;
                        D[i] = castOp(s0);
                        D[i+1] = castOp(s1);

                        s0 = (S0[i+2] + S2[i+2])*f1 + S1[i+2]*f0 + _delta;
                        s1 = (S0[i+3] + S2[i+3])*f1 + S1[i+3]*f0 + _delta;
                        D[i+2] = castOp(s0);
                        D[i+3] = castOp(s1);
                    }
                    #endif
                    for( ; i < width; i ++ )
                    {
                        ST s0 = (S0[i] + S2[i])*f1 + S1[i]*f0 + _delta;
                        D[i] = castOp(s0);
                    }
                }
            }
            else
            {
                if( is_m1_0_1 )
                {
                    if( f1 < 0 )
                        std::swap(S0, S2);
                    #if CV_ENABLE_UNROLLED
                    for( ; i <= width - 4; i += 4 )
                    {
                        ST s0 = S2[i] - S0[i] + _delta;
                        ST s1 = S2[i+1] - S0[i+1] + _delta;
                        D[i] = castOp(s0);
                        D[i+1] = castOp(s1);

                        s0 = S2[i+2] - S0[i+2] + _delta;
                        s1 = S2[i+3] - S0[i+3] + _delta;
                        D[i+2] = castOp(s0);
                        D[i+3] = castOp(s1);
                    }
                    #endif
                    for( ; i < width; i ++ )
                    {
                        ST s0 = S2[i] - S0[i] + _delta;
                        D[i] = castOp(s0);
                    }
                    if( f1 < 0 )
                        std::swap(S0, S2);
                }
                else
                {
                    #if CV_ENABLE_UNROLLED
                    for( ; i <= width - 4; i += 4 )
                    {
                        ST s0 = (S2[i] - S0[i])*f1 + _delta;
                        ST s1 = (S2[i+1] - S0[i+1])*f1 + _delta;
                        D[i] = castOp(s0);
                        D[i+1] = castOp(s1);

                        s0 = (S2[i+2] - S0[i+2])*f1 + _delta;
                        s1 = (S2[i+3] - S0[i+3])*f1 + _delta;
                        D[i+2] = castOp(s0);
                        D[i+3] = castOp(s1);
                    }
                    #endif
                    for( ; i < width; i++ )
                        D[i] = castOp((S2[i] - S0[i])*f1 + _delta);
                }
            }
        }
    }
};

template<typename ST, typename DT> struct Cast
{
    typedef ST type1;
    typedef DT rtype;

    DT operator()(ST val) const { return saturate_cast<DT>(val); }
};

template<typename ST, typename DT, int bits> struct FixedPtCast
{
    typedef ST type1;
    typedef DT rtype;
    enum { SHIFT = bits, DELTA = 1 << (bits-1) };

    DT operator()(ST val) const { return saturate_cast<DT>((val + DELTA)>>SHIFT); }
};

template<typename ST, typename DT> struct FixedPtCastEx
{
    typedef ST type1;
    typedef DT rtype;

    FixedPtCastEx() : SHIFT(0), DELTA(0) {}
    FixedPtCastEx(int bits) : SHIFT(bits), DELTA(bits ? 1 << (bits-1) : 0) {}
    DT operator()(ST val) const { return saturate_cast<DT>((val + DELTA)>>SHIFT); }
    int SHIFT, DELTA;
};

}

cv::Ptr<cv::BaseRowFilter> cv::getLinearRowFilter( int srcType, int bufType,
                                                   InputArray _kernel, int anchor,
                                                   int symmetryType )
{
    Mat kernel = _kernel.getMat();
    int sdepth = CV_MAT_DEPTH(srcType), ddepth = CV_MAT_DEPTH(bufType);
    int cn = CV_MAT_CN(srcType);
    CV_Assert( cn == CV_MAT_CN(bufType) &&
        ddepth >= std::max(sdepth, CV_32S) &&
        kernel.type() == ddepth );
    int ksize = kernel.rows + kernel.cols - 1;

    if( (symmetryType & (KERNEL_SYMMETRICAL|KERNEL_ASYMMETRICAL)) != 0 && ksize <= 5 )
    {
        if( sdepth == CV_8U && ddepth == CV_32S )
            return makePtr<SymmRowSmallFilter<uchar, int, SymmRowSmallVec_8u32s> >
                (kernel, anchor, symmetryType, SymmRowSmallVec_8u32s(kernel, symmetryType));
        if( sdepth == CV_32F && ddepth == CV_32F )
            return makePtr<SymmRowSmallFilter<float, float, SymmRowSmallVec_32f> >
                (kernel, anchor, symmetryType, SymmRowSmallVec_32f(kernel, symmetryType));
    }

    if( sdepth == CV_8U && ddepth == CV_32S )
        return makePtr<RowFilter<uchar, int, RowVec_8u32s> >
            (kernel, anchor, RowVec_8u32s(kernel));
    if( sdepth == CV_8U && ddepth == CV_32F )
        return makePtr<RowFilter<uchar, float, RowNoVec> >(kernel, anchor);
    if( sdepth == CV_8U && ddepth == CV_64F )
        return makePtr<RowFilter<uchar, double, RowNoVec> >(kernel, anchor);
    if( sdepth == CV_16U && ddepth == CV_32F )
        return makePtr<RowFilter<ushort, float, RowNoVec> >(kernel, anchor);
    if( sdepth == CV_16U && ddepth == CV_64F )
        return makePtr<RowFilter<ushort, double, RowNoVec> >(kernel, anchor);
    if( sdepth == CV_16S && ddepth == CV_32F )
        return makePtr<RowFilter<short, float, RowVec_16s32f> >
                                  (kernel, anchor, RowVec_16s32f(kernel));
    if( sdepth == CV_16S && ddepth == CV_64F )
        return makePtr<RowFilter<short, double, RowNoVec> >(kernel, anchor);
    if( sdepth == CV_32F && ddepth == CV_32F )
        return makePtr<RowFilter<float, float, RowVec_32f> >
            (kernel, anchor, RowVec_32f(kernel));
    if( sdepth == CV_32F && ddepth == CV_64F )
        return makePtr<RowFilter<float, double, RowNoVec> >(kernel, anchor);
    if( sdepth == CV_64F && ddepth == CV_64F )
        return makePtr<RowFilter<double, double, RowNoVec> >(kernel, anchor);

    CV_Error_( CV_StsNotImplemented,
        ("Unsupported combination of source format (=%d), and buffer format (=%d)",
        srcType, bufType));

    return Ptr<BaseRowFilter>();
}


cv::Ptr<cv::BaseColumnFilter> cv::getLinearColumnFilter( int bufType, int dstType,
                                             InputArray _kernel, int anchor,
                                             int symmetryType, double delta,
                                             int bits )
{
    Mat kernel = _kernel.getMat();
    int sdepth = CV_MAT_DEPTH(bufType), ddepth = CV_MAT_DEPTH(dstType);
    int cn = CV_MAT_CN(dstType);
    CV_Assert( cn == CV_MAT_CN(bufType) &&
        sdepth >= std::max(ddepth, CV_32S) &&
        kernel.type() == sdepth );

    if( !(symmetryType & (KERNEL_SYMMETRICAL|KERNEL_ASYMMETRICAL)) )
    {
        if( ddepth == CV_8U && sdepth == CV_32S )
            return makePtr<ColumnFilter<FixedPtCastEx<int, uchar>, ColumnNoVec> >
            (kernel, anchor, delta, FixedPtCastEx<int, uchar>(bits));
        if( ddepth == CV_8U && sdepth == CV_32F )
            return makePtr<ColumnFilter<Cast<float, uchar>, ColumnNoVec> >(kernel, anchor, delta);
        if( ddepth == CV_8U && sdepth == CV_64F )
            return makePtr<ColumnFilter<Cast<double, uchar>, ColumnNoVec> >(kernel, anchor, delta);
        if( ddepth == CV_16U && sdepth == CV_32F )
            return makePtr<ColumnFilter<Cast<float, ushort>, ColumnNoVec> >(kernel, anchor, delta);
        if( ddepth == CV_16U && sdepth == CV_64F )
            return makePtr<ColumnFilter<Cast<double, ushort>, ColumnNoVec> >(kernel, anchor, delta);
        if( ddepth == CV_16S && sdepth == CV_32F )
            return makePtr<ColumnFilter<Cast<float, short>, ColumnNoVec> >(kernel, anchor, delta);
        if( ddepth == CV_16S && sdepth == CV_64F )
            return makePtr<ColumnFilter<Cast<double, short>, ColumnNoVec> >(kernel, anchor, delta);
        if( ddepth == CV_32F && sdepth == CV_32F )
            return makePtr<ColumnFilter<Cast<float, float>, ColumnNoVec> >(kernel, anchor, delta);
        if( ddepth == CV_64F && sdepth == CV_64F )
            return makePtr<ColumnFilter<Cast<double, double>, ColumnNoVec> >(kernel, anchor, delta);
    }
    else
    {
        int ksize = kernel.rows + kernel.cols - 1;
        if( ksize == 3 )
        {
            if( ddepth == CV_8U && sdepth == CV_32S )
                return makePtr<SymmColumnSmallFilter<
                    FixedPtCastEx<int, uchar>, SymmColumnVec_32s8u> >
                    (kernel, anchor, delta, symmetryType, FixedPtCastEx<int, uchar>(bits),
                    SymmColumnVec_32s8u(kernel, symmetryType, bits, delta));
            if( ddepth == CV_16S && sdepth == CV_32S && bits == 0 )
                return makePtr<SymmColumnSmallFilter<Cast<int, short>,
                    SymmColumnSmallVec_32s16s> >(kernel, anchor, delta, symmetryType,
                        Cast<int, short>(), SymmColumnSmallVec_32s16s(kernel, symmetryType, bits, delta));
            if( ddepth == CV_32F && sdepth == CV_32F )
                return makePtr<SymmColumnSmallFilter<
                    Cast<float, float>,SymmColumnSmallVec_32f> >
                    (kernel, anchor, delta, symmetryType, Cast<float, float>(),
                    SymmColumnSmallVec_32f(kernel, symmetryType, 0, delta));
        }
        if( ddepth == CV_8U && sdepth == CV_32S )
            return makePtr<SymmColumnFilter<FixedPtCastEx<int, uchar>, SymmColumnVec_32s8u> >
                (kernel, anchor, delta, symmetryType, FixedPtCastEx<int, uchar>(bits),
                SymmColumnVec_32s8u(kernel, symmetryType, bits, delta));
        if( ddepth == CV_8U && sdepth == CV_32F )
            return makePtr<SymmColumnFilter<Cast<float, uchar>, ColumnNoVec> >
                (kernel, anchor, delta, symmetryType);
        if( ddepth == CV_8U && sdepth == CV_64F )
            return makePtr<SymmColumnFilter<Cast<double, uchar>, ColumnNoVec> >
                (kernel, anchor, delta, symmetryType);
        if( ddepth == CV_16U && sdepth == CV_32F )
            return makePtr<SymmColumnFilter<Cast<float, ushort>, ColumnNoVec> >
                (kernel, anchor, delta, symmetryType);
        if( ddepth == CV_16U && sdepth == CV_64F )
            return makePtr<SymmColumnFilter<Cast<double, ushort>, ColumnNoVec> >
                (kernel, anchor, delta, symmetryType);
        if( ddepth == CV_16S && sdepth == CV_32S )
            return makePtr<SymmColumnFilter<Cast<int, short>, ColumnNoVec> >
                (kernel, anchor, delta, symmetryType);
        if( ddepth == CV_16S && sdepth == CV_32F )
            return makePtr<SymmColumnFilter<Cast<float, short>, SymmColumnVec_32f16s> >
                 (kernel, anchor, delta, symmetryType, Cast<float, short>(),
                  SymmColumnVec_32f16s(kernel, symmetryType, 0, delta));
        if( ddepth == CV_16S && sdepth == CV_64F )
            return makePtr<SymmColumnFilter<Cast<double, short>, ColumnNoVec> >
                (kernel, anchor, delta, symmetryType);
        if( ddepth == CV_32F && sdepth == CV_32F )
            return makePtr<SymmColumnFilter<Cast<float, float>, SymmColumnVec_32f> >
                (kernel, anchor, delta, symmetryType, Cast<float, float>(),
                SymmColumnVec_32f(kernel, symmetryType, 0, delta));
        if( ddepth == CV_64F && sdepth == CV_64F )
            return makePtr<SymmColumnFilter<Cast<double, double>, ColumnNoVec> >
                (kernel, anchor, delta, symmetryType);
    }

    CV_Error_( CV_StsNotImplemented,
        ("Unsupported combination of buffer format (=%d), and destination format (=%d)",
        bufType, dstType));

    return Ptr<BaseColumnFilter>();
}



/****************************************************************************************\
*                               Non-separable linear filter                              *
\****************************************************************************************/

namespace cv
{

void preprocess2DKernel( const Mat& kernel, std::vector<Point>& coords, std::vector<uchar>& coeffs )
{
    int i, j, k, nz = countNonZero(kernel), ktype = kernel.type();
    if(nz == 0)
        nz = 1;
    CV_Assert( ktype == CV_8U || ktype == CV_32S || ktype == CV_32F || ktype == CV_64F );
    coords.resize(nz);
    coeffs.resize(nz*getElemSize(ktype));
    uchar* _coeffs = &coeffs[0];

    for( i = k = 0; i < kernel.rows; i++ )
    {
        const uchar* krow = kernel.ptr(i);
        for( j = 0; j < kernel.cols; j++ )
        {
            if( ktype == CV_8U )
            {
                uchar val = krow[j];
                if( val == 0 )
                    continue;
                coords[k] = Point(j,i);
                _coeffs[k++] = val;
            }
            else if( ktype == CV_32S )
            {
                int val = ((const int*)krow)[j];
                if( val == 0 )
                    continue;
                coords[k] = Point(j,i);
                ((int*)_coeffs)[k++] = val;
            }
            else if( ktype == CV_32F )
            {
                float val = ((const float*)krow)[j];
                if( val == 0 )
                    continue;
                coords[k] = Point(j,i);
                ((float*)_coeffs)[k++] = val;
            }
            else
            {
                double val = ((const double*)krow)[j];
                if( val == 0 )
                    continue;
                coords[k] = Point(j,i);
                ((double*)_coeffs)[k++] = val;
            }
        }
    }
}


template<typename ST, class CastOp, class VecOp> struct Filter2D : public BaseFilter
{
    typedef typename CastOp::type1 KT;
    typedef typename CastOp::rtype DT;

    Filter2D( const Mat& _kernel, Point _anchor,
        double _delta, const CastOp& _castOp=CastOp(),
        const VecOp& _vecOp=VecOp() )
    {
        anchor = _anchor;
        ksize = _kernel.size();
        delta = saturate_cast<KT>(_delta);
        castOp0 = _castOp;
        vecOp = _vecOp;
        CV_Assert( _kernel.type() == DataType<KT>::type );
        preprocess2DKernel( _kernel, coords, coeffs );
        ptrs.resize( coords.size() );
    }

    void operator()(const uchar** src, uchar* dst, int dststep, int count, int width, int cn)
    {
        KT _delta = delta;
        const Point* pt = &coords[0];
        const KT* kf = (const KT*)&coeffs[0];
        const ST** kp = (const ST**)&ptrs[0];
        int i, k, nz = (int)coords.size();
        CastOp castOp = castOp0;

        width *= cn;
        for( ; count > 0; count--, dst += dststep, src++ )
        {
            DT* D = (DT*)dst;

            for( k = 0; k < nz; k++ )
                kp[k] = (const ST*)src[pt[k].y] + pt[k].x*cn;

            i = vecOp((const uchar**)kp, dst, width);
            #if CV_ENABLE_UNROLLED
            for( ; i <= width - 4; i += 4 )
            {
                KT s0 = _delta, s1 = _delta, s2 = _delta, s3 = _delta;

                for( k = 0; k < nz; k++ )
                {
                    const ST* sptr = kp[k] + i;
                    KT f = kf[k];
                    s0 += f*sptr[0];
                    s1 += f*sptr[1];
                    s2 += f*sptr[2];
                    s3 += f*sptr[3];
                }

                D[i] = castOp(s0); D[i+1] = castOp(s1);
                D[i+2] = castOp(s2); D[i+3] = castOp(s3);
            }
            #endif
            for( ; i < width; i++ )
            {
                KT s0 = _delta;
                for( k = 0; k < nz; k++ )
                    s0 += kf[k]*kp[k][i];
                D[i] = castOp(s0);
            }
        }
    }

    std::vector<Point> coords;
    std::vector<uchar> coeffs;
    std::vector<uchar*> ptrs;
    KT delta;
    CastOp castOp0;
    VecOp vecOp;
};

}

cv::Ptr<cv::BaseFilter> cv::getLinearFilter(int srcType, int dstType,
                                InputArray filter_kernel, Point anchor,
                                double delta, int bits)
{
    Mat _kernel = filter_kernel.getMat();
    int sdepth = CV_MAT_DEPTH(srcType), ddepth = CV_MAT_DEPTH(dstType);
    int cn = CV_MAT_CN(srcType), kdepth = _kernel.depth();
    CV_Assert( cn == CV_MAT_CN(dstType) && ddepth >= sdepth );

    anchor = normalizeAnchor(anchor, _kernel.size());

    /*if( sdepth == CV_8U && ddepth == CV_8U && kdepth == CV_32S )
        return makePtr<Filter2D<uchar, FixedPtCastEx<int, uchar>, FilterVec_8u> >
            (_kernel, anchor, delta, FixedPtCastEx<int, uchar>(bits),
            FilterVec_8u(_kernel, bits, delta));
    if( sdepth == CV_8U && ddepth == CV_16S && kdepth == CV_32S )
        return makePtr<Filter2D<uchar, FixedPtCastEx<int, short>, FilterVec_8u16s> >
            (_kernel, anchor, delta, FixedPtCastEx<int, short>(bits),
            FilterVec_8u16s(_kernel, bits, delta));*/

    kdepth = sdepth == CV_64F || ddepth == CV_64F ? CV_64F : CV_32F;
    Mat kernel;
    if( _kernel.type() == kdepth )
        kernel = _kernel;
    else
        _kernel.convertTo(kernel, kdepth, _kernel.type() == CV_32S ? 1./(1 << bits) : 1.);

    if( sdepth == CV_8U && ddepth == CV_8U )
        return makePtr<Filter2D<uchar, Cast<float, uchar>, FilterVec_8u> >
            (kernel, anchor, delta, Cast<float, uchar>(), FilterVec_8u(kernel, 0, delta));
    if( sdepth == CV_8U && ddepth == CV_16U )
        return makePtr<Filter2D<uchar,
            Cast<float, ushort>, FilterNoVec> >(kernel, anchor, delta);
    if( sdepth == CV_8U && ddepth == CV_16S )
        return makePtr<Filter2D<uchar, Cast<float, short>, FilterVec_8u16s> >
            (kernel, anchor, delta, Cast<float, short>(), FilterVec_8u16s(kernel, 0, delta));
    if( sdepth == CV_8U && ddepth == CV_32F )
        return makePtr<Filter2D<uchar,
            Cast<float, float>, FilterNoVec> >(kernel, anchor, delta);
    if( sdepth == CV_8U && ddepth == CV_64F )
        return makePtr<Filter2D<uchar,
            Cast<double, double>, FilterNoVec> >(kernel, anchor, delta);

    if( sdepth == CV_16U && ddepth == CV_16U )
        return makePtr<Filter2D<ushort,
            Cast<float, ushort>, FilterNoVec> >(kernel, anchor, delta);
    if( sdepth == CV_16U && ddepth == CV_32F )
        return makePtr<Filter2D<ushort,
            Cast<float, float>, FilterNoVec> >(kernel, anchor, delta);
    if( sdepth == CV_16U && ddepth == CV_64F )
        return makePtr<Filter2D<ushort,
            Cast<double, double>, FilterNoVec> >(kernel, anchor, delta);

    if( sdepth == CV_16S && ddepth == CV_16S )
        return makePtr<Filter2D<short,
            Cast<float, short>, FilterNoVec> >(kernel, anchor, delta);
    if( sdepth == CV_16S && ddepth == CV_32F )
        return makePtr<Filter2D<short,
            Cast<float, float>, FilterNoVec> >(kernel, anchor, delta);
    if( sdepth == CV_16S && ddepth == CV_64F )
        return makePtr<Filter2D<short,
            Cast<double, double>, FilterNoVec> >(kernel, anchor, delta);

    if( sdepth == CV_32F && ddepth == CV_32F )
        return makePtr<Filter2D<float, Cast<float, float>, FilterVec_32f> >
            (kernel, anchor, delta, Cast<float, float>(), FilterVec_32f(kernel, 0, delta));
    if( sdepth == CV_64F && ddepth == CV_64F )
        return makePtr<Filter2D<double,
            Cast<double, double>, FilterNoVec> >(kernel, anchor, delta);

    CV_Error_( CV_StsNotImplemented,
        ("Unsupported combination of source format (=%d), and destination format (=%d)",
        srcType, dstType));

    return Ptr<BaseFilter>();
}


cv::Ptr<cv::FilterEngine> cv::createLinearFilter( int _srcType, int _dstType,
                                              InputArray filter_kernel,
                                              Point _anchor, double _delta,
                                              int _rowBorderType, int _columnBorderType,
                                              const Scalar& _borderValue )
{
    Mat _kernel = filter_kernel.getMat();
    _srcType = CV_MAT_TYPE(_srcType);
    _dstType = CV_MAT_TYPE(_dstType);
    int cn = CV_MAT_CN(_srcType);
    CV_Assert( cn == CV_MAT_CN(_dstType) );

    Mat kernel = _kernel;
    int bits = 0;

    /*int sdepth = CV_MAT_DEPTH(_srcType), ddepth = CV_MAT_DEPTH(_dstType);
    int ktype = _kernel.depth() == CV_32S ? KERNEL_INTEGER : getKernelType(_kernel, _anchor);
    if( sdepth == CV_8U && (ddepth == CV_8U || ddepth == CV_16S) &&
        _kernel.rows*_kernel.cols <= (1 << 10) )
    {
        bits = (ktype & KERNEL_INTEGER) ? 0 : 11;
        _kernel.convertTo(kernel, CV_32S, 1 << bits);
    }*/

    Ptr<BaseFilter> _filter2D = getLinearFilter(_srcType, _dstType,
        kernel, _anchor, _delta, bits);

    return makePtr<FilterEngine>(_filter2D, Ptr<BaseRowFilter>(),
        Ptr<BaseColumnFilter>(), _srcType, _dstType, _srcType,
        _rowBorderType, _columnBorderType, _borderValue );
}
#endif

/* End of file. */
