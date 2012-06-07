/* -------------------------------------------------------------------------- *
 *                                   OpenMM                                   *
 * -------------------------------------------------------------------------- *
 * This is part of the OpenMM molecular simulation toolkit originating from   *
 * Simbios, the NIH National Center for Physics-Based Simulation of           *
 * Biological Structures at Stanford, funded under the NIH Roadmap for        *
 * Medical Research, grant U54 GM072970. See https://simtk.org.               *
 *                                                                            *
 * Portions copyright (c) 2011-2012 Stanford University and the Authors.      *
 * Authors: Peter Eastman                                                     *
 * Contributors:                                                              *
 *                                                                            *
 * This program is free software: you can redistribute it and/or modify       *
 * it under the terms of the GNU Lesser General Public License as published   *
 * by the Free Software Foundation, either version 3 of the License, or       *
 * (at your option) any later version.                                        *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU Lesser General Public License for more details.                        *
 *                                                                            *
 * You should have received a copy of the GNU Lesser General Public License   *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.      *
 * -------------------------------------------------------------------------- */

#include "CudaBondedUtilities.h"
#include "CudaExpressionUtilities.h"
#include "openmm/OpenMMException.h"
#include "CudaNonbondedUtilities.h"
#include <iostream>

using namespace OpenMM;
using namespace std;

CudaBondedUtilities::CudaBondedUtilities(CudaContext& context) : context(context), numForceBuffers(0), maxBonds(0), hasInitializedKernels(false) {
}

CudaBondedUtilities::~CudaBondedUtilities() {
    for (int i = 0; i < (int) atomIndices.size(); i++)
        for (int j = 0; j < (int) atomIndices[i].size(); j++)
            delete atomIndices[i][j];
}

void CudaBondedUtilities::addInteraction(const vector<vector<int> >& atoms, const string& source, int group) {
    if (atoms.size() > 0) {
        forceAtoms.push_back(atoms);
        forceSource.push_back(source);
        forceGroup.push_back(group);
    }
}

std::string CudaBondedUtilities::addArgument(CUdeviceptr data, const string& type) {
    arguments.push_back(data);
    argTypes.push_back(type);
    return "customArg"+context.intToString(arguments.size());
}

void CudaBondedUtilities::addPrefixCode(const string& source) {
    prefixCode.push_back(source);
}

void CudaBondedUtilities::initialize(const System& system) {
    int numForces = forceAtoms.size();
    if (numForces == 0)
        return;
    
    // Build the lists of atom indices.
    
    atomIndices.resize(numForces);
    for (int i = 0; i < numForces; i++) {
        int numBonds = forceAtoms[i].size();
        int numAtoms = forceAtoms[i][0].size();
        int startAtom = 0;
        while (startAtom < numAtoms) {
            int width = max(numAtoms-startAtom, 4);
            if (width == 3)
                width = 4;
            vector<unsigned int> indexVec(width*numBonds);
            for (int bond = 0; bond < numBonds; bond++) {
                for (int atom = 0; atom < width; atom++)
                    indexVec[bond*width+atom] = forceAtoms[i][bond][startAtom+atom];
            }
            CudaArray* indices = CudaArray::create<unsigned int>(indexVec.size(), "bondedIndices");
            indices->upload(indexVec);
            atomIndices[i].push_back(indices);
            startAtom += width;
        }
    }

    // Create the kernel.

    stringstream s;
    for (int i = 0; i < (int) prefixCode.size(); i++)
        s<<prefixCode[i];
    s<<"extern \"C\" __global__ void computeBondedForces(long* __restrict__ forceBuffer, real* __restrict__ energyBuffer, const real4* __restrict__ posq, int groups";
    for (int force = 0; force < numForces; force++) {
        for (int i = 0; i < (int) atomIndices[force].size(); i++) {
            int indexWidth = atomIndices[force][i]->getElementSize()/4;
            string indexType = "unsigned int"+(indexWidth == 1 ? "" : context.intToString(indexWidth));
            s<<", const "<<indexType<<"* __restrict__ atomIndices"<<force<<"_"<<i;
        }
    }
    for (int i = 0; i < (int) arguments.size(); i++)
        s<<", "<<argTypes[i]<<"* customArg"<<(i+1);
    s<<") {\n";
    s<<"real energy = 0;\n";
    for (int force = 0; force < numForces; force++)
        s<<createForceSource(force, forceAtoms[force].size(), forceAtoms[force][0].size(), forceGroup[force], forceSource[force]);
    s<<"energyBuffer[blockIdx.x*blockDim.x+threadIdx.x] += energy;\n";
    s<<"}\n";
    map<string, string> defines;
    defines["PADDED_NUM_ATOMS"] = context.intToString(context.getPaddedNumAtoms());
    CUmodule module = context.createModule(s.str(), defines);
    kernel = context.getKernel(module, "computeBondedForces");
    forceAtoms.clear();
    forceSource.clear();
}

string CudaBondedUtilities::createForceSource(int forceIndex, int numBonds, int numAtoms, int group, const string& computeForce) {
    maxBonds = max(maxBonds, numBonds);
    string suffix1[] = {""};
    string suffix4[] = {".x", ".y", ".z", ".w"};
    string* suffix;
    stringstream s;
    s<<"if ((groups&"<<(1<<group)<<") != 0)\n";
    s<<"for (unsigned int index = blockIdx.x*blockDim.x+threadIdx.x; index < "<<numBonds<<"; index += blockDim.x*gridDim.x) {\n";
    int startAtom = 0;
    for (int i = 0; i < (int) atomIndices[forceIndex].size(); i++) {
        int indexWidth = atomIndices[forceIndex][i]->getElementSize()/4;
        suffix = (indexWidth == 1 ? suffix1 : suffix4);
        string indexType = "unsigned int"+(indexWidth == 1 ? "" : context.intToString(indexWidth));
        s<<"    "<<indexType<<" atoms"<<i<<" = atomIndices"<<forceIndex<<"_"<<i<<"[index];\n";
        s<<"    "<<indexType<<" buffers = bufferIndices"<<forceIndex<<"[index];\n";
        for (int j = 0; j < indexWidth; j++) {
            s<<"    unsigned int atom"<<(startAtom+j+1)<<" = atoms"<<i<<suffix[j]<<";\n";
            s<<"    real4 pos"<<(j+1)<<" = posq[atom"<<(j+1)<<"];\n";
        }
        startAtom += indexWidth;
    }
    s<<computeForce<<"\n";
    for (int i = 0; i < numAtoms; i++) {
        s<<"    atomicAdd(&forceBuffer[atom"<<(i+1)<<"], (long) (force.x*0xFFFFFFFF));\n";
        s<<"    atomicAdd(&forceBuffer[atom"<<(i+1)<<"+PADDED_NUM_ATOMS], (long) (force.x*0xFFFFFFFF));\n";
        s<<"    atomicAdd(&forceBuffer[atom"<<(i+1)<<"+PADDED_NUM_ATOMS*2], (long) (force.x*0xFFFFFFFF));\n";
    }
    s<<"}\n";
    return s.str();
}

void CudaBondedUtilities::computeInteractions(int groups) {
//    if (!hasInitializedKernels) {
//        hasInitializedKernels = true;
//        for (int i = 0; i < (int) forceSets.size(); i++) {
//            int index = 0;
//            cl::Kernel& kernel = kernels[i];
//            kernel.setArg<cl::Buffer>(index++, context.getForceBuffers().getDeviceBuffer());
//            kernel.setArg<cl::Buffer>(index++, context.getEnergyBuffer().getDeviceBuffer());
//            kernel.setArg<cl::Buffer>(index++, context.getPosq().getDeviceBuffer());
//            index++;
//            for (int j = 0; j < (int) forceSets[i].size(); j++) {
//                kernel.setArg<cl::Buffer>(index++, atomIndices[forceSets[i][j]]->getDeviceBuffer());
//                kernel.setArg<cl::Buffer>(index++, bufferIndices[forceSets[i][j]]->getDeviceBuffer());
//            }
//            for (int j = 0; j < (int) arguments.size(); j++)
//                kernel.setArg<cl::Memory>(index++, *arguments[j]);
//        }
//    }
//    for (int i = 0; i < (int) kernels.size(); i++) {
//        kernels[i].setArg<cl_int>(3, groups);
//        context.executeKernel(kernels[i], maxBonds);
//    }
}