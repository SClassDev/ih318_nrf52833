/** @brief Manufacturer specific advertising data structure */
// #define Manufacturer specific data
#define COMPANY_ID_CODE 0x0971
#define UUID_LENGTH 16


typedef struct adv_mfg_data {
	uint16_t companyId;
	uint32_t deviceDescriptor;
	uint8_t uuid_bytes[UUID_LENGTH]; /* Number of times Button 1 is pressed */
} adv_mfg_data_type;