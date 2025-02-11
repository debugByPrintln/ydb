#include "keyvalue_index_record.h"
#include "keyvalue_data.h"
#include "keyvalue_item_type.h"

namespace NKikimr {
namespace NKeyValue {

TIndexRecord::TChainItem::TChainItem(const TLogoBlobID &id, ui64 offset)
    : LogoBlobId(id)
    , Offset(offset)
{
}

TIndexRecord::TChainItem::TChainItem(TRcBuf&& inlineData, ui64 offset)
    : InlineData(std::move(inlineData))
    , Offset(offset)
{
}

bool TIndexRecord::TChainItem::IsInline() const {
    return !LogoBlobId.IsValid();
}

ui64 TIndexRecord::TChainItem::GetSize() const {
    if (LogoBlobId.IsValid()) {
        return LogoBlobId.BlobSize();
    } else {
        return InlineData.size();
    }
}

// ordering operator for LowerBound
bool operator <(ui64 left, const TIndexRecord::TChainItem& right) {
    return left < right.Offset;
}

// equlity operator for testing
bool TIndexRecord::TChainItem::operator==(const TIndexRecord::TChainItem& right) const {
    return LogoBlobId == right.LogoBlobId && Offset == right.Offset && InlineData == right.InlineData;
}

TIndexRecord::TIndexRecord()
    : CreationUnixTime(0)
{}

ui64 TIndexRecord::GetFullValueSize() const {
    return Chain.empty() ? 0 : Chain.back().Offset + Chain.back().GetSize();
}

ui32 TIndexRecord::GetReadItems(ui64 offset, ui64 size, TIntermediate::TRead& read) const {
    // for empty queries we issue no reads
    if (!size) {
        return 0;
    }
    auto it = UpperBound(Chain.begin(), Chain.end(), offset);
    Y_VERIFY(it != Chain.begin());
    --it;
    Y_VERIFY(offset >= it->Offset);
    offset -= it->Offset;

    ui64 valueOffset = 0;
    ui32 numReads = 0;
    while (size) {
        Y_VERIFY(it != Chain.end());
        ui32 readSize = Min<ui64>(size, it->GetSize() - offset);
        if (it->IsInline()) {
            if (read.Value.size() != read.ValueSize) {
                read.Value.resize(read.ValueSize);
            }
            Y_VERIFY(it->InlineData.size() >= readSize + offset, "size# %" PRIu64 " read# %" PRIu64 " offset# %" PRIu64,
                    (ui64)it->InlineData.size(), (ui64)readSize, (ui64)offset);
            Y_VERIFY(read.ValueSize >= readSize + valueOffset);
            memcpy(const_cast<char *>(read.Value.data()) + valueOffset, it->InlineData.data() + offset, readSize);
        } else {
            read.ReadItems.push_back(TIntermediate::TRead::TReadItem(
                    it->LogoBlobId, static_cast<ui32>(offset), readSize, valueOffset));
        }
        size -= readSize;
        offset = 0;
        valueOffset += readSize;
        ++it;
        ++numReads;
    }
    return numReads;
}

    // equlity operator for testing
bool TIndexRecord::operator==(const TIndexRecord& right) const {
    return Chain == right.Chain && CreationUnixTime == right.CreationUnixTime;
}

TString TIndexRecord::Serialize() const {
    TString value;
    ui64 totalSize = sizeof(TKeyValueData2);
    for (ui32 i = 0; i < Chain.size(); ++i) {
        if (Chain[i].IsInline()) {
            totalSize += sizeof(ui64) + sizeof(ui32) + Chain[i].InlineData.size();
        } else {
            totalSize += sizeof(TLogoBlobID);
        }
    }
    value.resize(totalSize);
    auto *data = reinterpret_cast<TKeyValueData2 *>(const_cast<char *>(value.data()));
    new(data) TKeyValueData2();
    data->CreationUnixTime = CreationUnixTime;

    ui64 offset = 0;
    for (ui32 i = 0; i < Chain.size(); ++i) {
        if (Chain[i].IsInline()) {
            memset(data->Serialized + offset, 0, sizeof(ui64));
            offset += sizeof(ui64);
            ui32 size = Chain[i].InlineData.size();
            memcpy(data->Serialized + offset, &size, sizeof(ui32));
            offset += sizeof(ui32);
            memcpy(data->Serialized + offset, Chain[i].InlineData.data(), Chain[i].InlineData.size());
            offset += Chain[i].InlineData.size();
        } else {
            memcpy(data->Serialized + offset, &Chain[i].LogoBlobId, sizeof(TLogoBlobID));
            offset += sizeof(TLogoBlobID);
        }
    }
    data->UpdateChecksum(totalSize);
    return value;
}

EItemType TIndexRecord::ReadItemType(const TString &rawData) {
    Y_VERIFY(rawData.size() >= sizeof(TDataHeader));
    const TDataHeader *dataHeader = (const TDataHeader *)rawData.data();
    return (EItemType)dataHeader->ItemType;
}

bool TIndexRecord::Deserialize1(const TString &rawData, TString &outErrorInfo) {
    Y_VERIFY(rawData.size() >= sizeof(TKeyValueData1));
    const TKeyValueData1 *data = (const TKeyValueData1 *)rawData.data();
    const ui32 numItems = TKeyValueData1::GetNumItems(rawData.size());
    if (!data->CheckChecksum(numItems)) {
        TStringStream str;
        str << " data->CheckChecksum(numItems)# ERROR ";
        str << " rawData.size# " << rawData.size();
        str << " numItems# " << numItems;
        str << " GetRecordSize# " << TKeyValueData1::GetRecordSize(numItems);
        str << " data# ";
        for (ui32 i = 0; i < rawData.size(); ++i) {
            ui8 d = ((const ui8*)rawData.data())[i];
            str << Sprintf("%02x", (ui32)d);
        }
        str << " FirstLogoBlobId# " << data->FirstLogoBlobId.ToString();
        str << " CreationUnixTime# " << data->CreationUnixTime;
        for (ui32 i = 1; i < numItems; ++i) {
            str << " ExtraIds[" << i << "]# " << data->ExtraIds[i - 1].ToString();
        }
        outErrorInfo = str.Str();
        return false;
    }
    Y_VERIFY(data->DataHeader.ItemType == EIT_KEYVALUE_1);
    ui64 offset = 0;
    if (data->FirstLogoBlobId) {
        Chain.push_back(TIndexRecord::TChainItem(data->FirstLogoBlobId, offset));
        offset += data->FirstLogoBlobId.BlobSize();
    }
    for (ui32 i = 1; i < numItems; ++i) {
        const TLogoBlobID& id = data->ExtraIds[i - 1];
        Chain.push_back(TIndexRecord::TChainItem(id, offset));
        offset += id.BlobSize();
    }
    CreationUnixTime = data->CreationUnixTime;
    return true;
}

bool TIndexRecord::Deserialize2(const TString &rawData, TString &outErrorInfo) {
    Y_VERIFY(rawData.size() >= sizeof(TKeyValueData2));
    const TKeyValueData2 *data = (const TKeyValueData2 *)rawData.data();
    if (!data->CheckChecksum(rawData.size())) {
        TStringStream str;
        str << " data->CheckChecksum(rawData.size)# ERROR ";
        str << " CreationUnixTime# " << data->CreationUnixTime;
        str << " rawData.size# " << rawData.size();
        str << " data# ";
        for (ui32 i = 0; i < rawData.size(); ++i) {
            ui8 d = ((const ui8*)rawData.data())[i];
            str << Sprintf("%02x", (ui32)d);
        }
        outErrorInfo = str.Str();
        return false;
    }
    Y_VERIFY(data->DataHeader.ItemType == EIT_KEYVALUE_2);
    CreationUnixTime = data->CreationUnixTime;
    ui64 chainOffset = 0;
    ui64 endOffset = rawData.size() - sizeof(TKeyValueData2);
    ui64 offset = 0;
    while (offset < endOffset) {
        if (endOffset - offset < sizeof(ui64)) {
            outErrorInfo = " Deserialization error# DEA1";
            return false;
        }
        ui64 temp;
        memcpy(&temp, data->Serialized + offset, sizeof(ui64));
        if (temp) {
            if (endOffset - offset < sizeof(TLogoBlobID)) {
                outErrorInfo = " Deserialization error# DEA2";
                return false;
            }
            TLogoBlobID id;
            memcpy(&id, data->Serialized + offset, sizeof(TLogoBlobID));
            offset += sizeof(TLogoBlobID);
            Chain.push_back(TIndexRecord::TChainItem(id, chainOffset));
            chainOffset += id.BlobSize();
        } else {
            offset += sizeof(ui64);
            if (endOffset - offset < sizeof(ui32)) {
                outErrorInfo = " Deserialization error# DEA3";
                return false;
            }
            ui32 size;
            memcpy(&size, data->Serialized + offset, sizeof(ui32));
            offset += sizeof(ui32);
            if (endOffset - offset < size) {
                outErrorInfo = " Deserialization error# DEA4";
                return false;
            }
            TRcBuf inlineData = TRcBuf::Uninitialized(size);
            memcpy(inlineData.GetDataMut(), data->Serialized + offset, size);
            offset += size;
            Chain.push_back(TIndexRecord::TChainItem(std::move(inlineData), chainOffset));
            chainOffset += size;
        }
    }
    return true;
}


} // NKeyValue
} // NKikimr
