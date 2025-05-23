// Copyright Contributors to the OpenImageIO project.
// SPDX-License-Identifier: Apache-2.0
// https://github.com/AcademySoftwareFoundation/OpenImageIO

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include <tsl/robin_map.h>

#include <OpenImageIO/dassert.h>
#include <OpenImageIO/deepdata.h>
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/fmath.h>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/parallel.h>
#include <OpenImageIO/strutil.h>
#include <OpenImageIO/typedesc.h>

#include "imageio_pvt.h"


OIIO_NAMESPACE_BEGIN
using namespace pvt;


// store an error message per thread, for a specific ImageInput
static thread_local tsl::robin_map<uint64_t, std::string> input_error_messages;
static std::atomic_int64_t input_next_id(0);


class ImageInput::Impl {
public:
    Impl()
        : m_id(++input_next_id)
        , m_threads(pvt::oiio_threads)
    {
    }

    // So we can lock this ImageInput for the thread-safe methods.
    std::recursive_mutex m_mutex;
    uint64_t m_id;
    int m_threads = 0;

    // The IOProxy object we will use for all I/O operations.
    Filesystem::IOProxy* m_io = nullptr;
    // The "local" proxy that we will create to use if the user didn't
    // supply a proxy for us to use.
    std::unique_ptr<Filesystem::IOProxy> m_io_local;
};



void
ImageInput::impl_deleter(Impl* impl)
{
    delete impl;
}



void*
ImageInput::operator new(size_t size)
{
    return ::operator new(size);
    // Note: if we ever need to guarantee alignment, we can change to:
    // return aligned_malloc (size, alignment);
}



void
ImageInput::operator delete(void* ptr)
{
    ImageInput* in = (ImageInput*)ptr;
    ::operator delete(in);
    // Note: if we ever need to guarantee alignment, we can change to:
    // aligned_free (ptr);
}



ImageInput::ImageInput()
    : m_impl(new Impl, impl_deleter)
{
}



ImageInput::~ImageInput()
{
    // Erase any leftover errors from this thread
    // TODO: can we clear other threads' errors?
    // TODO: potentially unsafe due to the static destruction order fiasco
    // input_error_messages.erase(m_impl->m_id);
}



// Default implementation of valid_file: try to do a full open.  If it
// succeeds, it's the right kind of file.  We assume that most plugins
// will override this with something smarter and much less expensive,
// like reading just the first few bytes of the file to check for magic
// numbers.
bool
ImageInput::valid_file(const std::string& filename) const
{
    ImageInput* self = const_cast<ImageInput*>(this);

    if (self->supports("ioproxy")) {
        Filesystem::IOFile io(filename, Filesystem::IOProxy::Read);
        return valid_file(&io);
    } else {
        ImageSpec tmpspec;
        bool ok = self->open(filename, tmpspec);
        if (ok)
            self->close();
        (void)geterror();  // Clear any errors
        return ok;
    }
}



bool
ImageInput::valid_file(Filesystem::IOProxy* ioproxy) const
{
    ImageInput* self = const_cast<ImageInput*>(this);

    /* Open using the ioproxy if possible. */
    if (!self->set_ioproxy(ioproxy))
        return false;

    ImageSpec config, tmpspec;
    bool ok = self->open("", tmpspec, config);
    if (ok) {
        self->close();
    }
    self->ioproxy_clear();

    (void)geterror();  // Clear any errors
    return ok;
}



std::unique_ptr<ImageInput>
ImageInput::open(const std::string& filename, const ImageSpec* config,
                 Filesystem::IOProxy* ioproxy)
{
    if (!config) {
        // Without config, this is really just a call to create-with-open.
        return ImageInput::create(filename, true, nullptr, ioproxy);
    }

    // With config, create without open, then try to open with config.
    auto in = ImageInput::create(filename, false, config, ioproxy);
    if (!in)
        return in;  // create() failed, return the empty ptr
    ImageSpec newspec;
    if (!in->open(filename, newspec, *config)) {
        // The open failed.  Transfer the error from 'in' to the global OIIO
        // error, delete the ImageInput we allocated, and return NULL.
        std::string err = in->geterror();
        if (err.size())
            OIIO::errorfmt("{}", err);
        in.reset();
    }

    return in;
}



ImageSpec
ImageInput::spec(int subimage, int miplevel)
{
    // This default base class implementation just locks, calls
    // seek_subimage, then copies the spec. But ImageInput subclass
    // implementations are free to do something more efficient, e.g. if they
    // already internally cache all of the subimage specs and thus don't
    // need a seek.
    ImageSpec ret;
    lock_guard lock(*this);
    if (seek_subimage(subimage, miplevel))
        ret = m_spec;
    return ret;
    // N.B. single return of named value should guaranteed copy elision.
}



ImageSpec
ImageInput::spec_dimensions(int subimage, int miplevel)
{
    // This default base class implementation just locks, calls
    // seek_subimage, then copies the spec. But ImageInput subclass
    // implementations are free to do something more efficient, e.g. if they
    // already internally cache all of the subimage specs and thus don't
    // need a seek.
    ImageSpec ret;
    lock_guard lock(*this);
    if (seek_subimage(subimage, miplevel))
        ret.copy_dimensions(m_spec);
    return ret;
    // N.B. single return of named value should guaranteed copy elision.
}



bool
ImageInput::read_scanline(int y, int z, TypeDesc format, void* data,
                          stride_t xstride)
{
    lock_guard lock(*this);

    // native_pixel_bytes is the size of a pixel in the FILE, including
    // the per-channel format.
    stride_t native_pixel_bytes = (stride_t)m_spec.pixel_bytes(true);
    // perchanfile is true if the file has different per-channel formats
    bool perchanfile = m_spec.channelformats.size();
    // native_data is true if the user asking for data in the native format
    bool native_data = (format == TypeDesc::UNKNOWN
                        || (format == m_spec.format && !perchanfile));
    // buffer_pixel_bytes is the size in the buffer
    stride_t buffer_pixel_bytes = native_data
                                      ? native_pixel_bytes
                                      : format.size() * m_spec.nchannels;
    if (native_data && xstride == AutoStride)
        xstride = native_pixel_bytes;
    else
        m_spec.auto_stride(xstride, format, m_spec.nchannels);
    // Do the strides indicate that the data area is contiguous?
    bool contiguous = (xstride == buffer_pixel_bytes);
    // Bytes a scanline takes in native formats
    size_t native_sl_bytes = m_spec.scanline_bytes(true);

    // If user's format and strides are set up to accept the native data
    // layout, read the scanline directly into the user's buffer.
    if (native_data && contiguous)
        return read_native_scanlines(current_subimage(), current_miplevel(), y,
                                     y + 1,
                                     as_writable_bytes(data, native_sl_bytes));

    // Complex case -- either changing data type or stride
    int scanline_values = m_spec.width * m_spec.nchannels;

    unsigned char* buf;
    OIIO_ALLOCATE_STACK_OR_HEAP(buf, unsigned char, native_sl_bytes);

    bool ok = read_native_scanlines(current_subimage(), current_miplevel(), y,
                                    y + 1,
                                    as_writable_bytes(buf, native_sl_bytes));
    if (!ok)
        return false;
    if (m_spec.channelformats.empty()) {
        // No per-channel formats -- do the conversion in one shot
        ok = contiguous ? convert_pixel_values(m_spec.format, buf, format, data,
                                               scanline_values)
                        : convert_image(m_spec.nchannels, m_spec.width, 1, 1,
                                        buf, m_spec.format, AutoStride,
                                        AutoStride, AutoStride, data, format,
                                        xstride, AutoStride, AutoStride);
    } else {
        // Per-channel formats -- have to convert/copy channels individually
        OIIO_DASSERT(m_spec.channelformats.size() == (size_t)m_spec.nchannels);
        size_t offset = 0;
        for (int c = 0; ok && c < m_spec.nchannels; ++c) {
            TypeDesc chanformat = m_spec.channelformats[c];
            ok = convert_image(1 /* channels */, m_spec.width, 1, 1,
                               buf + offset, chanformat, native_pixel_bytes,
                               AutoStride, AutoStride,
                               (char*)data + c * format.size(), format, xstride,
                               AutoStride, AutoStride);
            offset += chanformat.size();
        }
    }

    if (!ok)
        errorfmt("ImageInput::read_scanline : no support for format {}",
                 m_spec.format);
    return ok;
}



bool
ImageInput::read_scanlines(int subimage, int miplevel, int ybegin, int yend,
                           int z, int chbegin, int chend, TypeDesc format,
                           void* data, stride_t xstride, stride_t ystride)
{
    pvt::LoggedTimer logtime("II::read_scanlines");
    ImageSpec spec;
    int rps = 0;
    {
        // We need to lock briefly to retrieve rps from the spec
        lock_guard lock(*this);
        if (!seek_subimage(subimage, miplevel))
            return false;
        // Copying the dimensions of the designated subimage/miplevel to a
        // local `spec` means that we can release the lock!  (Calls to
        // read_native_* will internally lock again if necessary.)
        spec.copy_dimensions(m_spec);
        // For scanline files, we also need one piece of metadata
        if (!spec.tile_width)
            rps = m_spec.get_int_attribute("tiff:RowsPerStrip", 64);
        // FIXME: does the above search of metadata have a significant cost?
    }
    if (spec.image_bytes() < 1) {
        errorfmt("Invalid image size {} x {} ({} chans)", m_spec.width,
                 m_spec.height, m_spec.nchannels);
        return false;
    }

    chend                     = clamp(chend, chbegin + 1, spec.nchannels);
    int nchans                = chend - chbegin;
    yend                      = std::min(yend, spec.y + spec.height);
    size_t native_pixel_bytes = spec.pixel_bytes(chbegin, chend, true);
    imagesize_t native_scanline_bytes
        = clamped_mult64((imagesize_t)spec.width,
                         (imagesize_t)native_pixel_bytes);
    bool native        = (format == TypeDesc::UNKNOWN);
    size_t pixel_bytes = native ? native_pixel_bytes : format.size() * nchans;
    if (native && xstride == AutoStride)
        xstride = pixel_bytes;
    stride_t zstride = AutoStride;
    spec.auto_stride(xstride, ystride, zstride, format, nchans, spec.width,
                     spec.height);
    stride_t buffer_pixel_bytes    = native ? native_pixel_bytes
                                            : format.size() * nchans;
    stride_t buffer_scanline_bytes = native ? native_scanline_bytes
                                            : buffer_pixel_bytes * spec.width;
    bool contiguous                = (xstride == (stride_t)buffer_pixel_bytes
                       && ystride == (stride_t)buffer_scanline_bytes);

    auto dataspan = make_span(static_cast<std::byte*>(data),
                              buffer_scanline_bytes * size_t(yend - ybegin));

    // no_type_convert is true if asking for data in the native format
    bool no_type_convert = (format == spec.format
                            && spec.channelformats.empty());
    if ((native || no_type_convert) && contiguous) {
        if (chbegin == 0 && chend == spec.nchannels)
            return read_native_scanlines(subimage, miplevel, ybegin, yend,
                                         dataspan);
        else
            return read_native_scanlines(subimage, miplevel, ybegin, yend,
                                         chbegin, chend, dataspan);
    }

    // No such luck.  Read scanlines in chunks.

    // Split into reasonable chunks -- try to use around 64 MB, but
    // round up to a multiple of the TIFF rows per strip (or 64).
    int chunk = std::max(1, (1 << 26) / int(spec.scanline_bytes(true)));
    chunk     = std::max(chunk, int(oiio_read_chunk));
    chunk     = round_to_multiple(chunk, rps);
    std::unique_ptr<std::byte[]> buf(
        new std::byte[chunk * native_scanline_bytes]);
    auto bufspan = make_span(buf.get(), chunk * native_scanline_bytes);

    bool ok             = true;
    int scanline_values = spec.width * nchans;
    for (; ok && ybegin < yend; ybegin += chunk) {
        int y1 = std::min(ybegin + chunk, yend);
        ok &= read_native_scanlines(subimage, miplevel, ybegin, y1, chbegin,
                                    chend, bufspan);
        if (!ok)
            break;

        int nscanlines  = y1 - ybegin;
        int chunkvalues = scanline_values * nscanlines;
        if (spec.channelformats.empty()) {
            // No per-channel formats -- do the conversion in one shot
            if (contiguous) {
                ok = convert_pixel_values(spec.format, buf.get(), format, data,
                                          chunkvalues);
            } else {
                ok = parallel_convert_image(nchans, spec.width, nscanlines, 1,
                                            buf.get(), spec.format, AutoStride,
                                            AutoStride, AutoStride, data,
                                            format, xstride, ystride, zstride,
                                            threads());
            }
        } else {
            // Per-channel formats -- have to convert/copy channels individually
            size_t offset = 0;
            int n         = 1;
            for (int c = 0; ok && c < nchans; c += n) {
                TypeDesc chanformat = spec.channelformats[c + chbegin];
                // Try to do more than one channel at a time to improve
                // memory coherence, if there are groups of adjacent
                // channels needing the same data conversion.
                for (n = 1; c + n < nchans; ++n)
                    if (spec.channelformats[c + chbegin + n] != chanformat)
                        break;
                ok = parallel_convert_image(n /* channels */, spec.width,
                                            nscanlines, 1, &buf[offset],
                                            chanformat, native_pixel_bytes,
                                            AutoStride, AutoStride,
                                            (char*)data + c * format.size(),
                                            format, xstride, ystride, zstride,
                                            threads());
                offset += n * chanformat.size();
            }
        }
        if (!ok)
            errorfmt("ImageInput::read_scanlines : no support for format {}",
                     spec.format);
        data = (char*)data + ystride * nscanlines;
    }
    return ok;
}



bool
ImageInput::read_native_scanlines(int subimage, int miplevel, int ybegin,
                                  int yend, int z, void* data)
{
    // Base class implementation of read_native_scanlines just repeatedly
    // calls read_native_scanline, which is supplied by every plugin.
    // Only the hardcore ones will overload read_native_scanlines with
    // their own implementation.
    lock_guard lock(*this);
    size_t ystride = m_spec.scanline_bytes(true);
    yend           = std::min(yend, spec().y + spec().height);
    for (int y = ybegin; y < yend; ++y) {
        bool ok = read_native_scanline(subimage, miplevel, y, z, data);
        if (!ok)
            return false;
        data = (char*)data + ystride;
    }
    return true;
}



bool
ImageInput::read_native_scanlines(int subimage, int miplevel, int ybegin,
                                  int yend, int z, int chbegin, int chend,
                                  void* data)
{
    ImageSpec spec = spec_dimensions(subimage, miplevel);  // thread-safe
    if (spec.undefined())
        return false;

    // All-channel case just reduces to the simpler read_native_scanlines.
    if (chbegin == 0 && chend >= spec.nchannels)
        return read_native_scanlines(subimage, miplevel, ybegin, yend, z, data);

    // Base class implementation of read_native_scanlines (with channel
    // subset) just calls read_native_scanlines (all channels), and
    // copies the appropriate subset.
    size_t prefix_bytes   = spec.pixel_bytes(0, chbegin, true);
    size_t subset_bytes   = spec.pixel_bytes(chbegin, chend, true);
    size_t subset_ystride = size_t(spec.width) * subset_bytes;

    // Read all channels of the scanlines into a temp buffer.
    size_t native_pixel_bytes = spec.pixel_bytes(true);
    size_t native_ystride     = size_t(spec.width) * native_pixel_bytes;
    std::unique_ptr<char[]> buf(new char[native_ystride * (yend - ybegin)]);
    yend    = std::min(yend, spec.y + spec.height);
    bool ok = read_native_scanlines(subimage, miplevel, ybegin, yend, z,
                                    buf.get());
    if (!ok)
        return false;

    // Now copy out the subset of channels we want. We can do this in
    // parallel.
    // clang-format off
    parallel_for (0, yend-ybegin,
                  [&,subset_bytes,prefix_bytes,native_pixel_bytes](int64_t y){
        char *b = buf.get() + native_ystride * y;
        char *d = (char *)data + subset_ystride * y;
        for (int x = 0;  x < spec.width;  ++x)
            memcpy (d + subset_bytes*x,
                    &b[prefix_bytes+native_pixel_bytes*x], subset_bytes);
    });
    // clang-format on
    return true;
}



bool
ImageInput::read_native_scanlines(int subimage, int miplevel, int ybegin,
                                  int yend, span<std::byte> data)
{
    if (pvt::oiio_print_debug
#ifndef NDEBUG
        || true
#endif
    ) {
        // Only for debug builds or if OIIO "debug" is enabled at runtime,
        // validate that the span size.
        ImageSpec s = spec_dimensions(subimage, miplevel);
        if (!valid_raw_span_size(data, s, s.x, s.x + s.width, ybegin, yend))
            return false;
    }

    // Default implementation (for now): call the old pointer based flavor
    return read_native_scanlines(subimage, miplevel, ybegin, yend, 0,
                                 data.data());
}



bool
ImageInput::read_native_scanlines(int subimage, int miplevel, int ybegin,
                                  int yend, int chbegin, int chend,
                                  span<std::byte> data)
{
    if (pvt::oiio_print_debug
#ifndef NDEBUG
        || true
#endif
    ) {
        // Only for debug builds or if OIIO "debug" is enabled at runtime,
        // validate that the span size.
        ImageSpec s = spec_dimensions(subimage, miplevel);
        if (!valid_raw_span_size(data, s, s.x, s.x + s.width, ybegin, yend, 0,
                                 1, chbegin, chend))
            return false;
    }

    // Default implementation (for now): call the old pointer based flavor
    return read_native_scanlines(subimage, miplevel, ybegin, yend, 0, chbegin,
                                 chend, data.data());
}



bool
ImageInput::read_tile(int x, int y, int z, TypeDesc format, void* data,
                      stride_t xstride, stride_t ystride, stride_t zstride)
{
    lock_guard lock(*this);
    if (!m_spec.tile_width || ((x - m_spec.x) % m_spec.tile_width) != 0
        || ((y - m_spec.y) % m_spec.tile_height) != 0
        || ((z - m_spec.z) % m_spec.tile_depth) != 0)
        return false;  // coordinates are not a tile corner

    // native_pixel_bytes is the size of a pixel in the FILE, including
    // the per-channel format.
    stride_t native_pixel_bytes = (stride_t)m_spec.pixel_bytes(true);
    // perchanfile is true if the file has different per-channel formats
    bool perchanfile = m_spec.channelformats.size();
    // native_data is true if the user asking for data in the native format
    bool native_data = (format == TypeDesc::UNKNOWN
                        || (format == m_spec.format && !perchanfile));
    if (format == TypeDesc::UNKNOWN && xstride == AutoStride)
        xstride = native_pixel_bytes;
    m_spec.auto_stride(xstride, ystride, zstride, format, m_spec.nchannels,
                       m_spec.tile_width, m_spec.tile_height);
    stride_t buffer_pixel_bytes = native_data
                                      ? native_pixel_bytes
                                      : format.size() * m_spec.nchannels;
    // Do the strides indicate that the data area is contiguous?
    bool contiguous
        = xstride == buffer_pixel_bytes
          && (ystride == xstride * m_spec.tile_width
              && (zstride == ystride * m_spec.tile_height || zstride == 0));

    // If user's format and strides are set up to accept the native data
    // layout, read the tile directly into the user's buffer.
    if (native_data && contiguous)
        return read_native_tile(current_subimage(), current_miplevel(), x, y, z,
                                data);  // Simple case

    // Complex case -- either changing data type or stride
    size_t tile_values = (size_t)m_spec.tile_pixels() * m_spec.nchannels;

    std::unique_ptr<char[]> buf(new char[m_spec.tile_bytes(true)]);
    bool ok = read_native_tile(current_subimage(), current_miplevel(), x, y, z,
                               &buf[0]);
    if (!ok)
        return false;
    if (m_spec.channelformats.empty()) {
        // No per-channel formats -- do the conversion in one shot
        ok = contiguous ? convert_pixel_values(m_spec.format, &buf[0], format,
                                               data, tile_values)
                        : convert_image(m_spec.nchannels, m_spec.tile_width,
                                        m_spec.tile_height, m_spec.tile_depth,
                                        &buf[0], m_spec.format, AutoStride,
                                        AutoStride, AutoStride, data, format,
                                        xstride, ystride, zstride);
    } else {
        // Per-channel formats -- have to convert/copy channels individually
        OIIO_DASSERT(m_spec.channelformats.size() == (size_t)m_spec.nchannels);
        size_t offset = 0;
        for (int c = 0; c < m_spec.nchannels; ++c) {
            TypeDesc chanformat = m_spec.channelformats[c];
            ok = convert_image(1 /* channels */, m_spec.tile_width,
                               m_spec.tile_height, m_spec.tile_depth,
                               &buf[offset], chanformat, native_pixel_bytes,
                               AutoStride, AutoStride,
                               (char*)data + c * format.size(), format, xstride,
                               AutoStride, AutoStride);
            offset += chanformat.size();
        }
    }

    if (!ok)
        errorfmt("ImageInput::read_tile : no support for format {}",
                 m_spec.format);
    return ok;
}



bool
ImageInput::read_tiles(int subimage, int miplevel, int xbegin, int xend,
                       int ybegin, int yend, int zbegin, int zend, int chbegin,
                       int chend, TypeDesc format, void* data, stride_t xstride,
                       stride_t ystride, stride_t zstride)
{
    ImageSpec spec = spec_dimensions(subimage, miplevel);  // thread-safe
    if (spec.undefined())
        return false;

    chend = clamp(chend, chbegin + 1, spec.nchannels);
    if (!spec.valid_tile_range(xbegin, xend, ybegin, yend, zbegin, zend))
        return false;

    int nchans = chend - chbegin;
    // native_pixel_bytes is the size of a pixel in the FILE, including
    // the per-channel format.
    stride_t native_pixel_bytes = (stride_t)spec.pixel_bytes(chbegin, chend,
                                                             true);
    // perchanfile is true if the file has different per-channel formats
    bool perchanfile = spec.channelformats.size();
    // native_data is true if the user asking for data in the native format
    bool native_data = (format == TypeDesc::UNKNOWN
                        || (format == spec.format && !perchanfile));
    if (format == TypeDesc::UNKNOWN && xstride == AutoStride)
        xstride = native_pixel_bytes;
    spec.auto_stride(xstride, ystride, zstride, format, nchans, xend - xbegin,
                     yend - ybegin);
    // Do the strides indicate that the data area is contiguous?
    bool contiguous = (native_data && xstride == native_pixel_bytes)
                      || (!native_data
                          && xstride == (stride_t)spec.pixel_bytes(false));
    contiguous
        &= (ystride == xstride * (xend - xbegin)
            && (zstride == ystride * (yend - ybegin) || (zend - zbegin) <= 1));

    int nxtiles = (xend - xbegin + spec.tile_width - 1) / spec.tile_width;
    int nytiles = (yend - ybegin + spec.tile_height - 1) / spec.tile_height;
    int nztiles = (zend - zbegin + spec.tile_depth - 1) / spec.tile_depth;

    // If user's format and strides are set up to accept the native data
    // layout, and we're asking for a whole number of tiles (no partial
    // tiles at the edges), then read the tile directly into the user's
    // buffer.
    if (native_data && contiguous
        && (xend - xbegin) == nxtiles * spec.tile_width
        && (yend - ybegin) == nytiles * spec.tile_height
        && (zend - zbegin) == nztiles * spec.tile_depth) {
        if (chbegin == 0 && chend == spec.nchannels)
            return read_native_tiles(subimage, miplevel, xbegin, xend, ybegin,
                                     yend, zbegin, zend,
                                     data);  // Simple case
        else
            return read_native_tiles(subimage, miplevel, xbegin, xend, ybegin,
                                     yend, zbegin, zend, chbegin, chend, data);
    }

    // No such luck.  Just punt and read tiles individually.
    bool ok                        = true;
    stride_t pixelsize             = native_data ? native_pixel_bytes
                                                 : (format.size() * nchans);
    stride_t native_pixelsize      = spec.pixel_bytes(true);
    stride_t full_pixelsize        = native_data ? native_pixelsize
                                                 : (format.size() * spec.nchannels);
    stride_t full_tilewidthbytes   = full_pixelsize * spec.tile_width;
    stride_t full_tilewhbytes      = full_tilewidthbytes * spec.tile_height;
    stride_t full_tilebytes        = full_tilewhbytes * spec.tile_depth;
    stride_t full_native_tilebytes = spec.tile_bytes(true);
    size_t prefix_bytes = native_data ? spec.pixel_bytes(0, chbegin, true)
                                      : format.size() * chbegin;
    bool allchans       = (chbegin == 0 && chend == spec.nchannels);
    std::vector<char> buf;
    for (int z = zbegin; z < zend; z += std::max(1, spec.tile_depth)) {
        int zd      = std::min(zend - z, spec.tile_depth);
        bool full_z = (zd == spec.tile_depth);
        for (int y = ybegin; ok && y < yend; y += spec.tile_height) {
            char* tilestart = ((char*)data + (z - zbegin) * zstride
                               + (y - ybegin) * ystride);
            int yh          = std::min(yend - y, spec.tile_height);
            bool full_y     = (yh == spec.tile_height);
            int x           = xbegin;
            // If we're reading full y and z tiles and not doing any funny
            // business with channels, try to read as many complete x tiles
            // as we can in this row.
            int x_full_tiles = (xend - xbegin) / spec.tile_width;
            if (full_z && full_y && allchans && !perchanfile
                && x_full_tiles >= 1) {
                int x_full_tile_end = xbegin + x_full_tiles * spec.tile_width;
                if (buf.size() < size_t(full_native_tilebytes * x_full_tiles))
                    buf.resize(full_native_tilebytes * x_full_tiles);
                ok &= read_native_tiles(subimage, miplevel, xbegin,
                                        x_full_tile_end, y, y + yh, z, z + zd,
                                        chbegin, chend, &buf[0]);
                if (ok)
                    convert_image(nchans, x_full_tiles * spec.tile_width, yh,
                                  zd, &buf[0], spec.format, native_pixelsize,
                                  native_pixelsize * x_full_tiles
                                      * spec.tile_width,
                                  native_pixelsize * x_full_tiles
                                      * spec.tile_width * spec.tile_height,
                                  tilestart, format, xstride, ystride, zstride);
                tilestart += x_full_tiles * spec.tile_width * xstride;
                x += x_full_tiles * spec.tile_width;
            }

            // Now get the rest in the row, anything that is only a
            // partial tile, which needs extra care.
            // Since we are here relying on the non-thread-safe read_tile()
            // call, we re-establish the lock and make sure we're on the
            // right subimage/miplevel.
            for (; ok && x < xend; x += spec.tile_width) {
                int xw      = std::min(xend - x, spec.tile_width);
                bool full_x = (xw == spec.tile_width);
                // Full tiles are read directly into the user buffer,
                // but partial tiles (such as at the image edge) or
                // partial channel subsets are read into a buffer and
                // then copied.
                if (full_x && full_y && full_z && allchans && !perchanfile) {
                    // Full tile, either native data or not needing
                    // per-tile data format conversion.
                    lock_guard lock(*this);
                    if (!seek_subimage(subimage, miplevel))
                        return false;
                    ok &= read_tile(x, y, z, format, tilestart, xstride,
                                    ystride, zstride);
                    if (!ok)
                        return false;
                } else {
                    if (buf.size() < size_t(full_tilebytes))
                        buf.resize(full_tilebytes);
                    {
                        lock_guard lock(*this);
                        if (!seek_subimage(subimage, miplevel))
                            return false;
                        ok &= read_tile(x, y, z, format, &buf[0],
                                        full_pixelsize, full_tilewidthbytes,
                                        full_tilewhbytes);
                    }
                    if (ok)
                        copy_image(nchans, xw, yh, zd, &buf[prefix_bytes],
                                   pixelsize, full_pixelsize,
                                   full_tilewidthbytes, full_tilewhbytes,
                                   tilestart, xstride, ystride, zstride);
                    // N.B. It looks like read_tiles doesn't handle the
                    // per-channel data types case fully, but it does!
                    // The call to read_tile() above handles the case of
                    // per-channel data types, converting to to desired
                    // format, so all we have to do on our own is the
                    // copy_image.
                }
                tilestart += spec.tile_width * xstride;
            }
            if (!ok)
                break;
        }
    }

    return ok;
}



bool
ImageInput::read_native_tile(int /*subimage*/, int /*miplevel*/, int /*x*/,
                             int /*y*/, int /*z*/, void* /*data*/)
{
    // The base class read_native_tile fails. A format reader that supports
    // tiles MUST overload this virtual method that reads a single tile
    // (all channels).
    errorfmt("ImageInput::read_native_tile call unimplemented for {}",
             format_name());
    return false;
}


bool
ImageInput::read_native_tiles(int subimage, int miplevel, int xbegin, int xend,
                              int ybegin, int yend, int zbegin, int zend,
                              void* data)
{
    // A format reader that supports reading multiple tiles at once (in
    // a way that's more efficient than reading the tiles one at a time)
    // is advised (but not required) to overload this virtual method.
    // If an ImageInput subclass does not overload this, the default
    // implementation here is simply to loop over the tiles, calling the
    // single-tile read_native_tile() for each one.
    ImageSpec spec = spec_dimensions(subimage, miplevel);  // thread-safe
    if (spec.undefined())
        return false;
    if (!spec.valid_tile_range(xbegin, xend, ybegin, yend, zbegin, zend))
        return false;

    // Base class implementation of read_native_tiles just repeatedly
    // calls read_native_tile, which is supplied by every plugin that
    // supports tiles.  Only the hardcore ones will overload
    // read_native_tiles with their own implementation.
    stride_t pixel_bytes = (stride_t)spec.pixel_bytes(true);
    stride_t tileystride = pixel_bytes * spec.tile_width;
    stride_t tilezstride = tileystride * spec.tile_height;
    stride_t ystride     = (xend - xbegin) * pixel_bytes;
    stride_t zstride     = (yend - ybegin) * ystride;
    std::unique_ptr<char[]> pels(new char[spec.tile_bytes(true)]);
    for (int z = zbegin; z < zend; z += spec.tile_depth) {
        for (int y = ybegin; y < yend; y += spec.tile_height) {
            for (int x = xbegin; x < xend; x += spec.tile_width) {
                bool ok = read_native_tile(subimage, miplevel, x, y, z,
                                           &pels[0]);
                if (!ok)
                    return false;
                copy_image(spec.nchannels, spec.tile_width, spec.tile_height,
                           spec.tile_depth, &pels[0], size_t(pixel_bytes),
                           pixel_bytes, tileystride, tilezstride,
                           (char*)data + (z - zbegin) * zstride
                               + (y - ybegin) * ystride
                               + (x - xbegin) * pixel_bytes,
                           pixel_bytes, ystride, zstride);
            }
        }
    }
    return true;
}



bool
ImageInput::read_native_tiles(int subimage, int miplevel, int xbegin, int xend,
                              int ybegin, int yend, int zbegin, int zend,
                              int chbegin, int chend, void* data)
{
    // A format reader that supports reading multiple tiles at once, and can
    // handle a channel subset while doing so, is advised (but not required)
    // to overload this virtual method. If an ImageInput subclass does not
    // overload this, the default implementation here is simply to loop over
    // the tiles, calling the single-tile read_native_tile() for each one
    // (and copying carefully to handle the channel subset issues).
    ImageSpec spec = spec_dimensions(subimage, miplevel);  // thread-safe
    if (spec.undefined())
        return false;

    chend = clamp(chend, chbegin + 1, spec.nchannels);
    // All-channel case just reduces to the simpler version of
    // read_native_tiles.
    if (chbegin == 0 && chend >= spec.nchannels)
        return read_native_tiles(subimage, miplevel, xbegin, xend, ybegin, yend,
                                 zbegin, zend, data);
    // More complicated cases follow.

    if (!spec.valid_tile_range(xbegin, xend, ybegin, yend, zbegin, zend))
        return false;

    // Base class implementation of read_native_tiles just repeatedly
    // calls read_native_tile, which is supplied by every plugin that
    // supports tiles.  Only the hardcore ones will overload
    // read_native_tiles with their own implementation.

    int nchans                  = chend - chbegin;
    stride_t native_pixel_bytes = (stride_t)spec.pixel_bytes(true);
    stride_t native_tileystride = native_pixel_bytes * spec.tile_width;
    stride_t native_tilezstride = native_tileystride * spec.tile_height;

    size_t prefix_bytes     = spec.pixel_bytes(0, chbegin, true);
    size_t subset_bytes     = spec.pixel_bytes(chbegin, chend, true);
    stride_t subset_ystride = (xend - xbegin) * subset_bytes;
    stride_t subset_zstride = (yend - ybegin) * subset_ystride;

    std::unique_ptr<char[]> pels(new char[spec.tile_bytes(true)]);
    for (int z = zbegin; z < zend; z += spec.tile_depth) {
        for (int y = ybegin; y < yend; y += spec.tile_height) {
            for (int x = xbegin; x < xend; x += spec.tile_width) {
                bool ok = read_native_tile(subimage, miplevel, x, y, z,
                                           &pels[0]);
                if (!ok)
                    return false;
                copy_image(nchans, spec.tile_width, spec.tile_height,
                           spec.tile_depth, &pels[prefix_bytes], subset_bytes,
                           native_pixel_bytes, native_tileystride,
                           native_tilezstride,
                           (char*)data + (z - zbegin) * subset_zstride
                               + (y - ybegin) * subset_ystride
                               + (x - xbegin) * subset_bytes,
                           subset_bytes, subset_ystride, subset_zstride);
            }
        }
    }
    return true;
}



bool
ImageInput::read_native_tiles(int subimage, int miplevel, int xbegin, int xend,
                              int ybegin, int yend, span<std::byte> data)
{
    if (pvt::oiio_print_debug
#ifndef NDEBUG
        || true
#endif
    ) {
        // Only for debug builds or if OIIO "debug" is enabled at runtime,
        // validate that the span size.
        ImageSpec s = spec_dimensions(subimage, miplevel);
        if (!valid_raw_span_size(data, s, xbegin, xend, ybegin, yend))
            return false;
    }

    // Default implementation (for now): call the old pointer based flavor
    return read_native_tiles(subimage, miplevel, xbegin, xend, ybegin, yend, 0,
                             1, data.data());
}



bool
ImageInput::read_native_tiles(int subimage, int miplevel, int xbegin, int xend,
                              int ybegin, int yend, int chbegin, int chend,
                              span<std::byte> data)
{
    if (pvt::oiio_print_debug
#ifndef NDEBUG
        || true
#endif
    ) {
        // Only for debug builds or if OIIO "debug" is enabled at runtime,
        // validate that the span size is correct.
        ImageSpec s = spec_dimensions(subimage, miplevel);
        if (!valid_raw_span_size(data, s, xbegin, xend, ybegin, yend, 0, 1,
                                 chbegin, chend))
            return false;
    }

    // Default implementation (for now): call the old pointer based flavor
    return read_native_tiles(subimage, miplevel, xbegin, xend, ybegin, yend, 0,
                             1, chbegin, chend, data.data());
}



bool
ImageInput::read_native_volumetric_tiles(int subimage, int miplevel, int xbegin,
                                         int xend, int ybegin, int yend,
                                         int zbegin, int zend,
                                         span<std::byte> data)
{
    if (pvt::oiio_print_debug
#ifndef NDEBUG
        || true
#endif
    ) {
        // Only for debug builds or if OIIO "debug" is enabled at runtime,
        // validate that the span size.
        ImageSpec s = spec_dimensions(subimage, miplevel);
        if (!valid_raw_span_size(data, s, xbegin, xend, ybegin, yend, zbegin,
                                 zend))
            return false;
    }

    // Default implementation (for now): call the old pointer based flavor
    return read_native_tiles(subimage, miplevel, xbegin, xend, ybegin, yend,
                             zbegin, zend, data.data());
}



bool
ImageInput::read_native_volumetric_tiles(int subimage, int miplevel, int xbegin,
                                         int xend, int ybegin, int yend,
                                         int zbegin, int zend, int chbegin,
                                         int chend, span<std::byte> data)
{
    if (pvt::oiio_print_debug
#ifndef NDEBUG
        || true
#endif
    ) {
        // Only for debug builds or if OIIO "debug" is enabled at runtime,
        // validate that the span size.
        ImageSpec s = spec_dimensions(subimage, miplevel);
        if (!valid_raw_span_size(data, s, xbegin, xend, ybegin, yend, zbegin,
                                 zend, chbegin, chend))
            return false;
    }

    // Default implementation (for now): call the old pointer based flavor
    return read_native_tiles(subimage, miplevel, xbegin, xend, ybegin, yend,
                             zbegin, zend, chbegin, chend, data.data());
}



bool
ImageInput::read_image(int subimage, int miplevel, int chbegin, int chend,
                       TypeDesc format, void* data, stride_t xstride,
                       stride_t ystride, stride_t zstride,
                       ProgressCallback progress_callback,
                       void* progress_callback_data)
{
    pvt::LoggedTimer logtime("II::read_image");
    ImageSpec spec;
    int rps = 0;
    {
        // We need to lock briefly to retrieve rps from the spec
        lock_guard lock(*this);
        if (!seek_subimage(subimage, miplevel))
            return false;
        // Copying the dimensions of the designated subimage/miplevel to a
        // local `spec` means that we can release the lock!  (Calls to
        // read_native_* will internally lock again if necessary.)
        spec.copy_dimensions(m_spec);
        // For scanline files, we also need one piece of metadata
        if (!spec.tile_width)
            rps = m_spec.get_int_attribute("tiff:RowsPerStrip", 64);
    }
    if (spec.image_bytes() < 1) {
        errorfmt("Invalid image size {} x {} ({} chans)", m_spec.width,
                 m_spec.height, m_spec.nchannels);
        return false;
    }

    if (chend < 0)
        chend = spec.nchannels;
    chend                = clamp(chend, chbegin + 1, spec.nchannels);
    int nchans           = chend - chbegin;
    bool native          = (format == TypeDesc::UNKNOWN);
    stride_t pixel_bytes = native ? (stride_t)spec.pixel_bytes(chbegin, chend,
                                                               native)
                                  : (stride_t)(format.size() * nchans);
    if (native && xstride == AutoStride)
        xstride = pixel_bytes;
    spec.auto_stride(xstride, ystride, zstride, format, nchans, spec.width,
                     spec.height);
    bool ok = true;
    if (progress_callback)
        if (progress_callback(progress_callback_data, 0.0f))
            return ok;
    if (spec.tile_width) {  // Tiled image -- rely on read_tiles
        // Read in chunks of a whole row of tiles at once. If tiles are
        // 64x64, a 2k image has 32 tiles across. That's fine for now (for
        // parallelization purposes), but as typical core counts increase,
        // we may someday want to revisit this to batch multiple rows.
        for (int z = 0; z < spec.depth; z += spec.tile_depth) {
            for (int y = 0; y < spec.height && ok; y += spec.tile_height) {
                ok &= read_tiles(subimage, miplevel, spec.x,
                                 spec.x + spec.width, y + spec.y,
                                 std::min(y + spec.y + spec.tile_height,
                                          spec.y + spec.height),
                                 z + spec.z,
                                 std::min(z + spec.z + spec.tile_depth,
                                          spec.z + spec.depth),
                                 chbegin, chend, format,
                                 (char*)data + z * zstride + y * ystride,
                                 xstride, ystride, zstride);
                if (progress_callback
                    && progress_callback(progress_callback_data,
                                         (float)y / spec.height))
                    return ok;
            }
        }
    } else {  // Scanline image -- rely on read_scanlines.
        // Split into reasonable chunks -- try to use around 64 MB or the
        // oiio_read_chunk value, which ever is bigger, but also round up to
        // a multiple of the TIFF rows per strip (or 64).
        int chunk = std::max(1, (1 << 26) / int(spec.scanline_bytes(true)));
        chunk     = std::max(chunk, int(oiio_read_chunk));
        chunk     = round_to_multiple(chunk, rps);
        for (int z = 0; z < spec.depth; ++z) {
            for (int y = 0; y < spec.height && ok; y += chunk) {
                int yend = std::min(y + spec.y + chunk, spec.y + spec.height);
                ok &= read_scanlines(subimage, miplevel, y + spec.y, yend,
                                     z + spec.z, chbegin, chend, format,
                                     (char*)data + z * zstride + y * ystride,
                                     xstride, ystride);
                if (progress_callback)
                    if (progress_callback(progress_callback_data,
                                          (float)y / spec.height))
                        return ok;
            }
        }
    }
    if (progress_callback)
        progress_callback(progress_callback_data, 1.0f);
    return ok;
}



bool
ImageInput::read_native_deep_scanlines(int /*subimage*/, int /*miplevel*/,
                                       int /*ybegin*/, int /*yend*/, int /*z*/,
                                       int /*chbegin*/, int /*chend*/,
                                       DeepData& /*deepdata*/)
{
    return false;  // default: doesn't support deep images
}



bool
ImageInput::read_native_deep_tiles(int /*subimage*/, int /*miplevel*/,
                                   int /*xbegin*/, int /*xend*/, int /*ybegin*/,
                                   int /*yend*/, int /*zbegin*/, int /*zend*/,
                                   int /*chbegin*/, int /*chend*/,
                                   DeepData& /*deepdata*/)
{
    return false;  // default: doesn't support deep images
}



bool
ImageInput::read_native_deep_image(int subimage, int miplevel,
                                   DeepData& deepdata)
{
    ImageSpec spec = spec_dimensions(subimage, miplevel);  // thread-safe
    if (spec.undefined())
        return false;

    if (spec.depth > 1) {
        errorfmt(
            "read_native_deep_image is not supported for volume (3D) images.");
        return false;
        // FIXME? - not implementing 3D deep images for now.  The only
        // format that supports deep images at this time is OpenEXR, and
        // it doesn't support volumes.
    }
    if (spec.tile_width) {
        // Tiled image
        return read_native_deep_tiles(subimage, miplevel, spec.x,
                                      spec.x + spec.width, spec.y,
                                      spec.y + spec.height, spec.z,
                                      spec.z + spec.depth, 0, spec.nchannels,
                                      deepdata);
    } else {
        // Scanline image
        return read_native_deep_scanlines(subimage, miplevel, spec.y,
                                          spec.y + spec.height, 0, 0,
                                          spec.nchannels, deepdata);
    }
}



int
ImageInput::send_to_input(const char* /*format*/, ...)
{
    // FIXME -- I can't remember how this is supposed to work
    return 0;
}



int
ImageInput::send_to_client(const char* /*format*/, ...)
{
    // FIXME -- I can't remember how this is supposed to work
    return 0;
}



void
ImageInput::append_error(string_view message) const
{
    if (message.size() && message.back() == '\n')
        message.remove_suffix(1);
    std::string& err_str = input_error_messages[m_impl->m_id];
    OIIO_DASSERT(
        err_str.size() < 1024 * 1024 * 16
        && "Accumulated error messages > 16MB. Try checking return codes!");
    if (err_str.size() < 1024 * 1024 * 16) {
        if (err_str.size() && err_str.back() != '\n')
            err_str += '\n';
        err_str.append(message.begin(), message.end());
    }
}



bool
ImageInput::has_error() const
{
    auto iter = input_error_messages.find(m_impl->m_id);
    if (iter == input_error_messages.end())
        return false;
    return iter.value().size() > 0;
}



std::string
ImageInput::geterror(bool clear) const
{
    std::string e;
    auto iter = input_error_messages.find(m_impl->m_id);
    if (iter != input_error_messages.end()) {
        e = iter.value();
        if (clear)
            input_error_messages.erase(iter);
    }
    return e;
}



void
ImageInput::threads(int n)
{
    m_impl->m_threads = n;
}



int
ImageInput::threads() const
{
    return m_impl->m_threads;
}



void
ImageInput::lock() const
{
    m_impl->m_mutex.lock();
}



bool
ImageInput::try_lock() const
{
    return m_impl->m_mutex.try_lock();
}



void
ImageInput::unlock() const
{
    m_impl->m_mutex.unlock();
}


Filesystem::IOProxy*
ImageInput::ioproxy()
{
    return m_impl->m_io;
}



const Filesystem::IOProxy*
ImageInput::ioproxy() const
{
    return m_impl->m_io;
}



bool
ImageInput::set_ioproxy(Filesystem::IOProxy* ioproxy)
{
    m_impl->m_io = ioproxy;
    return (ioproxy == nullptr || supports("ioproxy"));
}



bool
ImageInput::ioproxy_opened() const
{
    Filesystem::IOProxy*& m_io(m_impl->m_io);
    return m_io != nullptr && m_io->mode() == Filesystem::IOProxy::Read;
}



void
ImageInput::ioproxy_clear()
{
    m_impl->m_io = nullptr;
    m_impl->m_io_local.reset();
}



void
ImageInput::ioproxy_retrieve_from_config(const ImageSpec& config)
{
    if (auto p = config.find_attribute("oiio:ioproxy", TypeDesc::PTR))
        set_ioproxy(p->get<Filesystem::IOProxy*>());
}



bool
ImageInput::ioproxy_use_or_open(string_view name)
{
    Filesystem::IOProxy*& m_io(m_impl->m_io);
    if (!m_io) {
        // If no proxy was supplied, create an IOFile
        m_io = new Filesystem::IOFile(name, Filesystem::IOProxy::Mode::Read);
        m_impl->m_io_local.reset(m_io);
    }
    if (!m_io || m_io->mode() != Filesystem::IOProxy::Mode::Read) {
        errorfmt("Could not open file \"{}\"", name);
        ioproxy_clear();
        return false;
    }
    return true;
}



bool
ImageInput::ioread(void* buf, size_t itemsize, size_t nitems)
{
    Filesystem::IOProxy*& m_io(m_impl->m_io);
    size_t size = itemsize * nitems;
    size_t n    = m_io->read(buf, size);
    if (n != size) {
        if (size_t(m_io->tell()) >= m_io->size())
            ImageInput::errorfmt("Read error: hit end of file in {} reader",
                                 format_name());
        else
            ImageInput::errorfmt(
                "Read error at position {}, could only read {}/{} bytes {}",
                m_io->tell() - n, n, size, m_io->error());
    }
    return n == size;
}



bool
ImageInput::ioseek(int64_t pos, int origin)
{
    Filesystem::IOProxy*& m_io(m_impl->m_io);
    if (m_io && !m_io->seek(pos, origin)) {
        errorfmt("Seek error, could not seek from {} to {} (total size {}) {}",
                 m_io->tell(),
                 origin == SEEK_SET ? pos
                                    : (origin == SEEK_CUR ? pos + m_io->tell()
                                                          : pos + m_io->size()),
                 m_io->size(), m_io->error());
        return false;
    }
    return true;
}



int64_t
ImageInput::iotell() const
{
    return m_impl->m_io->tell();
}



bool
ImageInput::check_open(const ImageSpec& spec, ROI range, uint64_t /*flags*/)
{
    // Check for sensible resolutions, etc.
    if ((m_spec.width <= 0 || m_spec.height <= 0 || m_spec.depth <= 0
         || m_spec.nchannels <= 0)
        && !supports("noimage")) {
        errorfmt(
            "{} image resolution must be at least 1x1, but the file specified {}x{}. Possible corrupt input?",
            format_name(), m_spec.width, m_spec.height);
        return false;
    }
    if (m_spec.depth > 1) {
        if (m_spec.width > range.width() || m_spec.height > range.height()
            || m_spec.depth > range.depth()) {
            errorfmt(
                "{} image resolution may not exceed {}x{}x{}, but the file appears to be {}x{}x{}. Possible corrupt input?",
                format_name(), range.width(), range.height(), range.depth(),
                m_spec.width, m_spec.height, m_spec.depth);
            return false;
        }
    } else {
        if (m_spec.width > range.width() || m_spec.height > range.height()) {
            errorfmt(
                "{} image resolution may not exceed {}x{}, but the file appears to be {}x{}. Possible corrupt input?",
                format_name(), range.width(), range.height(), m_spec.width,
                m_spec.height);
            return false;
        }
    }
    if (spec.nchannels > range.nchannels()) {
        errorfmt(
            "{} does not support {}-channel images. Possible corrupt input?",
            format_name(), spec.nchannels);
        return false;
    }
    if (pvt::limit_channels && spec.nchannels > pvt::limit_channels) {
        errorfmt(
            "{} channels exceeds \"limits:channels\" = {}. Possible corrupt input?\nIf you're sure this is a valid file, raise the OIIO global attribute \"limits:channels\".",
            spec.nchannels, pvt::limit_channels);
        return false;
    }
    if (pvt::limit_imagesize_MB
        && spec.image_bytes(true)
               > pvt::limit_imagesize_MB * imagesize_t(1024 * 1024)) {
        errorfmt(
            "Uncompressed image size {:.1f} MB exceeds the {} MB limit.\n"
            "Image claimed to be {}x{}, {}-channel {}. Possible corrupt input?\n"
            "If this is a valid file, raise the OIIO attribute \"limits:imagesize_MB\".",
            float(m_spec.image_bytes(true)) / float(1024 * 1024),
            pvt::limit_imagesize_MB, m_spec.width, m_spec.height,
            m_spec.nchannels, m_spec.format);
        return false;
    }

    return true;  // all is ok
}



bool
ImageInput::valid_raw_span_size(cspan<std::byte> buf, const ImageSpec& spec,
                                int xbegin, int xend, int ybegin, int yend,
                                int zbegin, int zend, int chbegin, int chend)
{
    if (chend < 0 || chend > spec.nchannels)
        chend = spec.nchannels;
    size_t sz = spec.pixel_bytes(chbegin, chend, true /*native*/)
                * size_t(xend - xbegin) * size_t(yend - ybegin)
                * size_t(zend - zbegin);
    if (sz > buf.size()) {
        errorfmt(
            "Buffer span size is inadequate to read the requested {} pixels ({} bytes vs {} needed)",
            format_name(), buf.size(), sz);
        return false;
    }
    return true;
}



template<>
inline size_t
pvt::heapsize<ImageInput::Impl>(const ImageInput::Impl& impl)
{
    return impl.m_io_local ? sizeof(Filesystem::IOProxy) : 0;
}



size_t
ImageInput::heapsize() const
{
    size_t size = pvt::heapsize(m_impl);
    size += pvt::heapsize(m_spec);
    return size;
}



size_t
ImageInput::footprint() const
{
    return sizeof(ImageInput) + heapsize();
}



template<>
size_t
pvt::heapsize<ImageInput>(const ImageInput& input)
{
    return input.heapsize();
}



template<>
size_t
pvt::footprint<ImageInput>(const ImageInput& input)
{
    return input.footprint();
}



OIIO_NAMESPACE_END
