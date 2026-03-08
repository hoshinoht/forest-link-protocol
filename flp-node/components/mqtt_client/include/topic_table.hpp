#pragma once

#include <cstdint>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace flp
{

struct TopicEntry
{
    uint16_t topic_id;
    char topic_name[64];
    bool registered;
};

class TopicTable
{
  public:
    TopicTable()
    {
        mutex_ = xSemaphoreCreateMutex();
        memset(entries_, 0, sizeof(entries_));
    }

    uint16_t register_topic(const char *name)
    {
        if (!mutex_ || !name)
        {
            return 0;
        }
        if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE)
        {
            return 0;
        }

        /* Check if topic already exists */
        for (uint8_t i = 0; i < count_; i++)
        {
            if (entries_[i].registered &&
                strncmp(entries_[i].topic_name,
                        name,
                        sizeof(entries_[i].topic_name)) == 0)
            {
                uint16_t id = entries_[i].topic_id;
                (void) xSemaphoreGive(mutex_);
                return id;
            }
        }

        /* Add new entry if space available */
        if (count_ >= MAX_TOPICS)
        {
            (void) xSemaphoreGive(mutex_);
            return 0; /* table full */
        }

        uint16_t new_id = next_id_++;
        entries_[count_].topic_id = new_id;
        strncpy(entries_[count_].topic_name,
                name,
                sizeof(entries_[count_].topic_name) - 1);
        entries_[count_].topic_name[sizeof(entries_[count_].topic_name) - 1] =
            '\0';
        entries_[count_].registered = true;
        count_++;

        (void) xSemaphoreGive(mutex_);
        return new_id;
    }

    const char *lookup(uint16_t topic_id) const
    {
        if (!mutex_)
        {
            return nullptr;
        }
        if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE)
        {
            return nullptr;
        }

        for (uint8_t i = 0; i < count_; i++)
        {
            if (entries_[i].registered && entries_[i].topic_id == topic_id)
            {
                const char *name = entries_[i].topic_name;
                (void) xSemaphoreGive(mutex_);
                return name;
            }
        }

        (void) xSemaphoreGive(mutex_);
        return nullptr;
    }

  private:
    static constexpr uint8_t MAX_TOPICS = 16;
    TopicEntry entries_[MAX_TOPICS];
    uint8_t count_ = 0;
    uint16_t next_id_ = 1;
    mutable SemaphoreHandle_t mutex_ = nullptr;
};

} /* namespace flp */
