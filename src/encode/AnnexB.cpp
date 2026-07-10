#include "encode/AnnexB.h"

namespace od {

namespace {

struct Marker {
    size_t codeBegin;
    size_t payloadBegin;
};

std::vector<Marker> FindMarkers(const uint8_t* data, size_t size)
{
    std::vector<Marker> markers;
    size_t i = 0;
    while (i + 1 < size) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                markers.push_back({i, i + 4});
                i += 4;
                continue;
            }
            if (i + 2 < size && data[i + 2] == 1) {
                markers.push_back({i, i + 3});
                i += 3;
                continue;
            }
        }
        ++i;
    }
    return markers;
}

} // namespace

std::vector<NalUnit> ScanStartCodes(const uint8_t* data, size_t size)
{
    std::vector<NalUnit> result;
    if (data == nullptr || size == 0)
        return result;

    auto markers = FindMarkers(data, size);
    for (size_t idx = 0; idx < markers.size(); ++idx) {
        size_t payloadBegin = markers[idx].payloadBegin;
        size_t payloadEnd = (idx + 1 < markers.size()) ? markers[idx + 1].codeBegin : size;
        if (payloadBegin >= payloadEnd)
            continue;

        NalUnit nal;
        nal.type = data[payloadBegin] & 0x1F;
        nal.payload.assign(data + payloadBegin, data + payloadEnd);
        result.push_back(std::move(nal));
    }
    return result;
}

std::vector<uint8_t> BuildAccessUnit(const std::vector<NalUnit>& nals)
{
    std::vector<uint8_t> out;
    for (const auto& nal : nals) {
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0x01);
        out.insert(out.end(), nal.payload.begin(), nal.payload.end());
    }
    return out;
}

std::vector<NalUnit> SpsPpsCache::EnsureParameterSets(const std::vector<NalUnit>& nals)
{
    for (const auto& nal : nals) {
        if (nal.type == kNalTypeSps)
            sps_ = nal;
        else if (nal.type == kNalTypePps)
            pps_ = nal;
    }

    int idrIndex = -1;
    for (size_t i = 0; i < nals.size(); ++i) {
        if (nals[i].type == kNalTypeIdrSlice) {
            idrIndex = static_cast<int>(i);
            break;
        }
    }
    if (idrIndex < 0)
        return nals; // not a keyframe access unit, nothing to ensure

    bool hasSpsBefore = false;
    bool hasPpsBefore = false;
    for (int i = 0; i < idrIndex; ++i) {
        if (nals[i].type == kNalTypeSps) hasSpsBefore = true;
        if (nals[i].type == kNalTypePps) hasPpsBefore = true;
    }
    if (hasSpsBefore && hasPpsBefore)
        return nals;

    if (!sps_ || !pps_)
        return nals; // nothing cached yet to fix up with (first-ever frame must supply its own)

    std::vector<NalUnit> result;
    result.reserve(nals.size() + 2);
    result.push_back(*sps_);
    result.push_back(*pps_);
    result.insert(result.end(), nals.begin(), nals.end());
    return result;
}

} // namespace od
