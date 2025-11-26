#include <iostream>
#include <vector>

// FONDAMENTALE per Windows
#define H5_BUILT_AS_DYNAMIC_LIB
#include "hdf5.h"

#define FILE_NAME "data_sz3_l-3.h5"
#define DATASET_NAME "simulation_data"

int main() {
    // --- CONFIGURAZIONE AMBIENTE ---

    // 1. Dice a HDF5 dove trovare il plugin (cartella corrente)
    _putenv("HDF5_PLUGIN_PATH=.");

    // 2. [FIX FREEZE] Disabilita il multi-threading di SZ3 per evitare blocchi su Windows
    _putenv("OMP_NUM_THREADS=1");

    std::cout << "--- LETTURA DEFINITIVA (SAFE MODE) ---" << std::endl;

    // Apertura File
    hid_t file_id = H5Fopen(FILE_NAME, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file_id < 0) {
        std::cout << "ERRORE: Impossibile aprire il file." << std::endl;
        system("pause"); return -1;
    }

    // Apertura Dataset
    hid_t dset_id = H5Dopen(file_id, DATASET_NAME, H5P_DEFAULT);
    if (dset_id < 0) {
        std::cout << "ERRORE: Dataset non trovato." << std::endl;
        H5Fclose(file_id);
        system("pause"); return -1;
    }

    // Analisi Dimensioni
    hid_t file_space = H5Dget_space(dset_id);
    int rank_int = H5Sget_simple_extent_ndims(file_space);
    size_t rank = static_cast<size_t>(rank_int);

    std::vector<hsize_t> dims(rank);
    H5Sget_simple_extent_dims(file_space, dims.data(), NULL);

    std::cout << "Geometria: [ ";
    for (size_t i = 0; i < rank; i++) std::cout << dims[i] << " ";
    std::cout << "]" << std::endl;

    // --- CONFIGURAZIONE LETTURA (2 RIGHE) ---
    // Target: Leggere le prime 2 righe complete del primo strato (Z=0)

    std::vector<hsize_t> offset(rank, 0);
    std::vector<hsize_t> count(rank, 1);

    if (rank >= 2) {
        // Ultima dim (X): Leggiamo tutto (2048)
        count[rank - 1] = dims[rank - 1];
        // Penultima dim (Y): Leggiamo 2 righe
        count[rank - 2] = 2;
    }
    else {
        count[0] = 2;
    }

    unsigned long long total_elements = 1;
    for (size_t i = 0; i < rank; i++) total_elements *= count[i];

    std::cout << "Leggo " << total_elements << " valori..." << std::endl;

    // Selezione Iperslab
    H5Sselect_hyperslab(file_space, H5S_SELECT_SET, offset.data(), NULL, count.data(), NULL);

    // Memoria
    hid_t mem_space = H5Screate_simple(static_cast<int>(rank), count.data(), NULL);
    std::vector<float> data(total_elements);

    std::cout << "Decompressione in corso (Single Thread)..." << std::endl;
    std::cout.flush();

    // --- ESECUZIONE ---
    herr_t status = H5Dread(dset_id, H5T_NATIVE_FLOAT, mem_space, file_space, H5P_DEFAULT, data.data());

    if (status < 0) {
        std::cout << "\n[ERRORE CRITICO] Lettura fallita." << std::endl;
    }
    else {
        std::cout << "\n[SUCCESS] Dati letti!" << std::endl;

        // Stampa Riga 1 (Primi 10 valori)
        std::cout << "\n--- RIGA 1 (Primi 10 valori) ---" << std::endl;
        for (int i = 0; i < 10; i++) std::cout << data[i] << " ";

        // Stampa Riga 2 (Primi 10 valori)
        if (rank >= 2) {
            size_t row_stride = count[rank - 1];
            std::cout << "\n\n--- RIGA 2 (Primi 10 valori) ---" << std::endl;
            for (int i = 0; i < 10; i++) std::cout << data[row_stride + i] << " ";
        }
        std::cout << std::endl;
    }

    H5Sclose(mem_space);
    H5Sclose(file_space);
    H5Dclose(dset_id);
    H5Fclose(file_id);

    std::cout << "\nPremi INVIO per uscire.";
    std::cin.get();
    return 0;
}