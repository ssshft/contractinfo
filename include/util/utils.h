#pragma once
#include "data_struct.h"

std::string mapToJsonArray(const std::unordered_map<std::string, md::InstrumentInfo>& mAllInfo) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    
    writer.StartArray();  // 开始数组
    
    for (const auto& [instId, info] : mAllInfo) {
        // 开始每个InstrumentInfo对象
        writer.StartObject();
        
        // 枚举字段
        writer.Key("exchangeTypeEnum");
        writer.Int(static_cast<int>(info.exchangeTypeEnum));
        
        writer.Key("instTypeEnum");
        writer.Int(static_cast<int>(info.instTypeEnum));
        
        // 字符串字段
        writer.Key("instId");
        writer.String(info.instId);
        
        writer.Key("originInstId");
        writer.String(info.originInstId);
        
        writer.Key("base");
        writer.String(info.base);
        
        writer.Key("quote");
        writer.String(info.quote);
        
        writer.Key("margin");
        writer.String(info.margin);
        
        // double字段
        writer.Key("value");
        writer.Double(info.value);
        
        writer.Key("tickSize");
        writer.Double(info.tickSize);
        
        writer.Key("lotSize");
        writer.Double(info.lotSize);
        
        writer.Key("minSize");
        writer.Double(info.minSize);
        
        writer.Key("maxSize");
        writer.Double(info.maxSize);
        
        writer.Key("minAmount");
        writer.Double(info.minAmount);
        
        writer.Key("magnifyNumber");
        writer.Double(info.magnifyNumber);
        
        writer.Key("reduceNumber");
        writer.Double(info.reduceNumber);
        
        writer.EndObject();  // 结束InstrumentInfo对象
    }
    
    writer.EndArray();  // 结束数组
    
    return buffer.GetString();
}