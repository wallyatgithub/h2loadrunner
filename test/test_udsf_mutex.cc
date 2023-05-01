#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include "udsf_data_store.h"

auto& realm = udsf::get_realm("realm1");
auto& storage = realm.get_storage("storage1");
std::vector<std::string> record_names;
std::vector<std::string> multipartBodies;
size_t number_records = 5;

void add_record()
{
    auto before_timepoint = std::chrono::steady_clock::now();
    while (true)
    {
        for (size_t i = 0; i < number_records; i++)
        {
            auto ret = storage.insert_or_update_record(record_names[i], "-----MULTIPART_BOUNDARY---", multipartBodies[i]);
            //std::cout << "insert_or_update_record ret:" << ret << std::endl;
            //storage.update_record_meta(record_names[i],
            //                           R"([{ "op": "replace", "path": "/tags/ueId", "value": ["450005"] }, { "op": "remove", "path": "/tags/recordId" }])");
            //std::cout << "record:" << storage.get_record("record1") << std::endl;
            //storage.get_record(record_names[i]);
            //std::cout << "number of record with recordId tag:" << storage.count_records_of_tag_name("schema_1", "recordId") << std::endl;
        }
        static size_t count = 0;
        count++;
        
        if (count % 10000 == 0)
        {
            std::cout << "add thread is alive"<<std::endl;
        }
    }
    auto after_timepoint = std::chrono::steady_clock::now();

    std::cout << "each record add cost:" << std::chrono::duration_cast<std::chrono::microseconds>
              (after_timepoint - before_timepoint).count() / number_records << std::endl;
    std::cout << "number of record with recordId tag:" << storage.count_records_of_tag_name("schema_1",
                                                                                            "recordId") << std::endl;
}

void get_record()
{
    auto before_timepoint = std::chrono::steady_clock::now();
    while (true)
    {
        for (size_t i = 0; i < number_records; i++)
        {
            storage.get_record(record_names[i]);
        }
        static size_t count = 0;
        count++;
        
        if (count % 1000000 == 0)
        {
            std::cout << "get thread is alive"<<std::endl;
        }
    }
    auto after_timepoint = std::chrono::steady_clock::now();

    //std::cout << "each record get cost:" << std::chrono::duration_cast<std::chrono::microseconds>
    //          (after_timepoint - before_timepoint).count() / number_records << std::endl;
    std::cout << "number of record with recordId tag:" << storage.count_records_of_tag_name("schema_1",
                                                                                            "recordId") << std::endl;
}

void delete_record()
{
    //std::cout << "number of record with recordId tag:" << storage.count_records_of_tag_name("schema_1", "recordId") << std::endl;;
    auto before_timepoint = std::chrono::steady_clock::now();
    while (true)
    {
        for (size_t i = 0; i < number_records; i++)
        {
            storage.delete_record(record_names[i]);
            //std::cout<<"delete record ret:"<<ret<<std::endl;
            //std::cout << "number of record with recordId tag:" << storage.count_records_of_tag_name("schema_1", record_names[i]) << std::endl;;
        }
        static size_t count = 0;
        count++;
        
        if (count % 1000000 == 0)
        {
            std::cout << "delete thread is alive"<<std::endl;
        }
    }
    auto after_timepoint = std::chrono::steady_clock::now();

    //std::cout << "each record delete cost:" << std::chrono::duration_cast<std::chrono::microseconds>
    //          (after_timepoint - before_timepoint).count() / number_records << std::endl;
    std::cout << "number of record with recordId tag:" << storage.count_records_of_tag_name("schema_1",
                                                                                            "recordId") << std::endl;;
}

int main(void)
{
    auto schema = R"(
{
  "schemaId": "schema_1",
  "metaTags": [
    {
      "tagName": "ueId",
      "keyType": "UNIQUE_KEY",
      "sort": true,
      "presence": true
    },
    {
      "tagName": "hashId",
      "keyType": "UNIQUE_KEY",
      "sort": false,
      "presence": false
    },
    {
      "tagName": "recordId",
      "keyType": "UNIQUE_KEY",
      "sort": false,
      "presence": true
    }
  ]
}
)";

    for (size_t i = 0; i < number_records; i++)
    {
        record_names.emplace_back(std::string("record").append(std::to_string(i)));
        std::string multipartBody;
        multipartBody += "-----MULTIPART_BOUNDARY---\r\n";
        multipartBody += "Content-Disposition: json-data; name=\"first_part\"\r\n";
        multipartBody += "Content-id: meta\r\n";
        multipartBody += "Content-Type: application/json\r\n";
        multipartBody += "\r\n";
        multipartBody += "{\"schemaId\": \"schema_1\",\"tags\":{\"ueId\" : [ \"";
        multipartBody += std::to_string(10000+i);
        multipartBody += "\", \"";
        multipartBody += std::to_string(20000+i);
        multipartBody += "\" ], \"recordId\" : [ \"";
        multipartBody += std::to_string(30000+i);
        multipartBody += "\" ] }}\r\n";
        multipartBody += "-----MULTIPART_BOUNDARY---\r\n";
        multipartBody += "Content-Disposition: form-data; name=\"first_part\"\r\n";
        multipartBody += "Content-id: block1\r\n";
        multipartBody += "Content-Type: text\r\n";
        multipartBody += "\r\n";
        multipartBody += "This is the first part of the HTTP body.\r\n";
        multipartBody += "-----MULTIPART_BOUNDARY---\r\n";
        multipartBody += "Content-Disposition: form-data; name=\"second_part\"\r\n";
        multipartBody += "Content-id: block2\r\n";
        multipartBody += "Content-Type: text\r\n";
        multipartBody += "\r\n";
        multipartBody += "This is the second part of the HTTP body.\r\n";
        multipartBody += "-----MULTIPART_BOUNDARY-----\r\n";
        multipartBodies.emplace_back(std::move(multipartBody));
    }

    storage.add_or_update_schema("schema_1", schema);
    std::thread add_thread(add_record);
    std::thread get_thread(get_record);
    std::thread delete_thread(delete_record);
    add_thread.join();
    get_thread.join();
    delete_thread.join();
    //add_record();
    //get_record();
    //delete_record();
    return 0;
}