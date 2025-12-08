#include <iostream>
#include <vector>
#include <algorithm> // Serve per std::min

#define H5_BUILT_AS_DYNAMIC_LIB
#include "hdf5.h"

// Usa il nome del tuo file NON compresso o compresso, funziona per entrambi
//#define FILE_NAME "data_sz3_l-3.h5" 
//#define DATASET_NAME "simulation_data"
#define FILE_NAME "processdata.h5"
#define DATASET_NAME "vertex/y"
int main() {
    _putenv("HDF5_PLUGIN_PATH=.");
    _putenv("OMP_NUM_THREADS=1"); // Sempre utile per sicurezza

    std::cout << "--- LETTURA SICURA (ADATTIVA) ---" << std::endl;

    // 1. Apertura
    hid_t file_id = H5Fopen(FILE_NAME, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file_id < 0) {
        std::cout << "ERRORE: File non trovato." << std::endl;
        system("pause"); return -1;
    }

    hid_t dset_id = H5Dopen(file_id, DATASET_NAME, H5P_DEFAULT);
    if (dset_id < 0) {
        std::cout << "ERRORE: Dataset non trovato." << std::endl;
        H5Fclose(file_id); return -1;
    }

    // 2. Analisi Dimensioni
    hid_t file_space = H5Dget_space(dset_id);
    int rank_int = H5Sget_simple_extent_ndims(file_space);
    size_t rank = static_cast<size_t>(rank_int);

    std::vector<hsize_t> dims(rank);
    H5Sget_simple_extent_dims(file_space, dims.data(), NULL);

    std::cout << "Geometria file: [ ";
    for (size_t i = 0; i < rank; i++) std::cout << dims[i] << " ";
    std::cout << "]" << std::endl;

    // 3. CONFIGURAZIONE LETTURA (Sicura)
    std::vector<hsize_t> offset(rank, 0);
    std::vector<hsize_t> count(rank, 1);

    // LOGICA: "Prova a leggere 2 righe, ma se ce n'è una sola, leggine una sola."
    if (rank >= 2) {
        // Ultima dimensione (X): Leggi tutto
        count[rank - 1] = dims[rank - 1];

        // Penultima dimensione (Y): Leggi 2 OPPURE il massimo disponibile
        // Se dims[..] è 1, leggerà 1. Se è 2048, leggerà 2.
        count[rank - 2] = (dims[rank - 2] >= 2) ? 2 : dims[rank - 2];
    }
    else {
        // Se è 1D, leggi fino a 10 elementi
        count[0] = (dims[0] >= 10) ? 10 : dims[0];
    }

    unsigned long long total_elements = 1;
    for (size_t i = 0; i < rank; i++) total_elements *= count[i];

    std::cout << "Leggo " << total_elements << " valori..." << std::endl;

    // 4. SELEZIONE
    H5Sselect_hyperslab(file_space, H5S_SELECT_SET, offset.data(), NULL, count.data(), NULL);
    hid_t mem_space = H5Screate_simple(static_cast<int>(rank), count.data(), NULL);

    std::vector<float> data(total_elements);

    // 5. ESECUZIONE
    std::cout << "Lettura in corso..." << std::endl;
    herr_t status = H5Dread(dset_id, H5T_NATIVE_FLOAT, mem_space, file_space, H5P_DEFAULT, data.data());

    if (status < 0) {
        std::cout << "\n[ERRORE] Lettura fallita." << std::endl;
    }
    else {
        std::cout << "\n[SUCCESS] Dati letti!" << std::endl;

        // Stampa anteprima
        std::cout << "Primi 5 valori:" << std::endl;
        size_t limit = (total_elements > 5) ? 5 : total_elements;
        for (int i = 0; i < limit; i++) std::cout << data[i] << " ";
        std::cout << "..." << std::endl;
    }

    H5Sclose(mem_space);
    H5Sclose(file_space);
    H5Dclose(dset_id);
    H5Fclose(file_id);


    std::cout << "\nPremi INVIO per uscire.";
    std::cin.get();
    return 0;
}