#ifndef __LIBIMDMA_H
#define __LIBIMDMA_H


typedef void imdma_t;
typedef void imdma_transfer_t;


/// @brief Create and open the given imdma device (/dev/imdma_...)
/// @param devicePath
/// @return imdma_t pointer on success; or NULL on failure
/// @note If this function returns non-NULL, the user must call imdma_free() when they are done with it
imdma_t *imdma_create(const char *devicePath);

/// @brief Clean up and free the given structure
/// @param imdma A pointer to the imdma_t returned by imdma_create()
void imdma_free(imdma_t *imdma);


/// @brief Allocate a buffer for a DMA transfer
/// @details If this function is unable to allocate a transfer buffer, NULL will be returned.
/// @param imdma A pointer to the imdma_t returned by imdma_create()
/// @note If this function returns non-NULL, the user must call imdma_transfer_free() when finished with it
/// @return imdma_transfer_t A pointer to the tracking data structure for the transfer; or NULL on failure
imdma_transfer_t *imdma_transfer_alloc(imdma_t *imdma);

/// @brief Request to start a DMA transfer of the given length
/// @note imdma_transfer_set_length() must be called first
///       In addition, outgoing transfers should also set the data using imdma_transfer_get_data()
/// @param lengthBytes The desired transfer length in bytes
/// @return 0 on success; or non-zero on error
int imdma_transfer_start_async(imdma_transfer_t *transfer);

/// @brief Wait for the given DMA transfer to finish
/// @param transfer A pointer to the imdma_transfer_t returned by imdma_transfer_alloc()
/// @return 0 on success; or non-zero on error
int imdma_transfer_finish_async(imdma_transfer_t *transfer);

/// @brief Free the given DMA transfer
/// @param transfer A pointer to the imdma_transfer_t returned by imdma_transfer_alloc()
/// @note This function must not be called until the user has finished using the data
void imdma_transfer_free(imdma_transfer_t *transfer);

/// @brief Set the length (in bytes) for the given DMA transfer
/// @param transfer A pointer to the imdma_transfer_t returned by imdma_transfer_alloc()
int imdma_transfer_set_length(imdma_transfer_t *transfer, unsigned int lengthBytes);

/// @brief Get the length (in bytes) of the given DMA transfer
/// @param transfer A pointer to the imdma_transfer_t returned by imdma_transfer_alloc()
unsigned int imdma_transfer_get_length(imdma_transfer_t *transfer);

/// @brief Set the maximum time (milliseconds) to wait for a transfer to complete
/// @param transfer A pointer to the imdma_transfer_t returned by imdma_transfer_alloc()
int imdma_transfer_set_timeout_ms(imdma_transfer_t *transfer, unsigned int timeoutMs);

/// @brief Get a (const) pointer to the data
/// @note For incoming transfers, imdma_transfer_finish_async() must be called first
/// @param transfer A pointer to the imdma_transfer_t returned by imdma_transfer_alloc()
const void *imdma_transfer_get_data_const(imdma_transfer_t *transfer);

/// @brief Get a pointer to the data
/// @note For outgoing transfers, this must be called before imdma_transfer_start_async()
/// @param transfer A pointer to the imdma_transfer_t returned by imdma_transfer_alloc()
void *imdma_transfer_get_data(imdma_transfer_t *transfer);

#endif
