/*
    Copyright (c) 2011, Philipp Krähenbühl
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
        * Redistributions of source code must retain the above copyright
        notice, this list of conditions and the following disclaimer.
        * Redistributions in binary form must reproduce the above copyright
        notice, this list of conditions and the following disclaimer in the
        documentation and/or other materials provided with the distribution.
        * Neither the name of the Stanford University nor the
        names of its contributors may be used to endorse or promote products
        derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY Philipp Krähenbühl ''AS IS'' AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL Philipp Krähenbühl BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "densecrf.hh"
#include "fastmath.hh"
#include "permutohedral.hh"
#include "dense_util.hh"
#include <cmath>
#include <cstring>




PairwisePotential::~PairwisePotential() {
}
SemiMetricFunction::~SemiMetricFunction() {
}
class PottsPotential: public PairwisePotential{
protected:
  Permutohedral lattice_;
  PottsPotential( const PottsPotential&o ){}
  int N_;
  float w_;
  float *norm_;
public:
  ~PottsPotential(){
    deallocate( norm_ );
  }
  PottsPotential(const float* features, int D, int N, float w, bool per_pixel_normalization=true) :N_(N), w_(w) {
    lattice_.init( features, D, N );
    norm_ = allocate( N );
    for ( int i=0; i<N; i++ )
      norm_[i] = 1;
    // Compute the normalization factor
    lattice_.compute( norm_, norm_, 1 );
    if ( per_pixel_normalization ) {
      // use a per pixel normalization
      for ( int i=0; i<N; i++ )
        norm_[i] = 1.0 / (norm_[i]+1e-20);;
    }
    else {
      float mean_norm = 0;
      for ( int i=0; i<N; i++ )
        mean_norm += norm_[i];
      mean_norm = N / mean_norm;
      // use a per pixel normalization
      for ( int i=0; i<N; i++ )
        norm_[i] = mean_norm;
    }
  }
  void apply(float* out_values, const float* in_values, float* tmp, int value_size) const {
    lattice_.compute( tmp, in_values, value_size );
    for ( int i=0,k=0; i<N_; i++ )
      for ( int j=0; j<value_size; j++, k++ )
        out_values[k] += w_*norm_[i]*tmp[k];
  }
};
class SemiMetricPotential: public PottsPotential{
protected:
  const SemiMetricFunction * function_;
public:
  void apply(float* out_values, const float* in_values, float* tmp, int value_size) const {
    lattice_.compute( tmp, in_values, value_size );

    // To the metric transform
    float * tmp2 = new float[value_size];
    for ( int i=0; i<N_; i++ ) {
      float * out = out_values + i*value_size;
      float * t1  = tmp  + i*value_size;
      function_->apply( tmp2, t1, value_size );
      for ( int j=0; j<value_size; j++ )
        out[j] -= w_*norm_[i]*tmp2[j];
    }
    delete[] tmp2;
  }
  SemiMetricPotential(const float* features, int D, int N, float w, const SemiMetricFunction* function, bool per_pixel_normalization=true) :PottsPotential( features, D, N, w, per_pixel_normalization ),function_(function) {
  }
};
/////////////////////////////
/////  Alloc / Dealloc  /////
/////////////////////////////
DenseCRF::DenseCRF(int N, int M) : N_(N), M_(M) {
  unary_ = allocate( N_*M_ );
  additional_unary_ = allocate( N_*M_ );
  current_ = allocate( N_*M_ );
  next_ = allocate( N_*M_ );
  next_temp_ = allocate( N_*M_ );
  tmp_ = allocate( 2*N_*M_ );
  // Set the additional_unary_ to zero
  memset( additional_unary_, 0, sizeof(float)*N_*M_ );
}
DenseCRF::~DenseCRF() {
  deallocate( unary_ );
  deallocate( additional_unary_ );
  deallocate( current_ );
  deallocate( next_ );
  deallocate( next_temp_ );
  deallocate( tmp_ );
  for( unsigned int i=0; i<pairwise_.size(); i++ )
    delete pairwise_[i];
}
DenseCRF2D::DenseCRF2D(int W, int H, int M) : DenseCRF(W*H,M), W_(W), H_(H) {
  m_do_not_parallelize = false;
}
DenseCRF2D::~DenseCRF2D() {
}
/////////////////////////////////
/////  Pairwise Potentials  /////
/////////////////////////////////
void DenseCRF::addPairwiseEnergy (const float* features, int D, float w, const SemiMetricFunction * function) {
  if (function)
    addPairwiseEnergy( new SemiMetricPotential( features, D, N_, w, function ) );
  else
    addPairwiseEnergy( new PottsPotential( features, D, N_, w ) );
}
void DenseCRF::addPairwiseEnergy ( PairwisePotential* potential ){
  pairwise_.push_back( potential );
}
void DenseCRF2D::addPairwiseGaussian ( float sx, float sy, float w, const SemiMetricFunction * function ) {
  float * feature = new float [N_*2];
  for( int j=0; j<H_; j++ )
    for( int i=0; i<W_; i++ ){
      feature[(j*W_+i)*2+0] = i / sx;
      feature[(j*W_+i)*2+1] = j / sy;
    }
  addPairwiseEnergy( feature, 2, w, function );
  delete [] feature;
}
void DenseCRF2D::addPairwiseBilateral ( float sx, float sy, float sr, float sg, float sb, const unsigned char* im, float w, const SemiMetricFunction * function ) {
  float * feature = new float [N_*5];
  for( int j=0; j<H_; j++ )
    for( int i=0; i<W_; i++ ){
      feature[(j*W_+i)*5+0] = i / sx;
      feature[(j*W_+i)*5+1] = j / sy;
      feature[(j*W_+i)*5+2] = im[(i+j*W_)*3+0] / sr;
      feature[(j*W_+i)*5+3] = im[(i+j*W_)*3+1] / sg;
      feature[(j*W_+i)*5+4] = im[(i+j*W_)*3+2] / sb;
    }
  addPairwiseEnergy( feature, 5, w, function );
  delete [] feature;
}


void DenseCRF2D::addPairwiseBilateral ( float sc, const unsigned char* im, float sd, const float* depth_im, float w, const SemiMetricFunction * function ) {
  float * feature = new float [N_*6];
  for( int j=0; j<H_; j++ )
    for( int i=0; i<W_; i++ ){
      feature[(j*W_+i)*6+0] = depth_im[(i+j*W_)*3+0] / sd;
      feature[(j*W_+i)*6+1] = depth_im[(i+j*W_)*3+1] / sd;
      feature[(j*W_+i)*6+2] = depth_im[(i+j*W_)*3+2] / sd;
      feature[(j*W_+i)*6+3] = im[(i+j*W_)*3+0] / sc;
      feature[(j*W_+i)*6+4] = im[(i+j*W_)*3+1] / sc;
      feature[(j*W_+i)*6+5] = im[(i+j*W_)*3+2] / sc;

    }
  addPairwiseEnergy( feature, 6, w, function );
  delete [] feature;
}

void DenseCRF2D::Add3dSmoothness(float sd, const float* x3, const float* y3, const float* z3, float w, const SemiMetricFunction * function ){
  float sd_inv = 1.0/sd;
  float * feature = new float [N_*3];
  for(int i=0; i < N_; ++i){
    feature[i*3  ] = x3[i] *sd_inv;
    feature[i*3+1] = y3[i] *sd_inv;
    feature[i*3+2] = z3[i] *sd_inv;
  }
  addPairwiseEnergy( feature, 3, w, function );
  delete [] feature;
}

void DenseCRF2D::Add3dAppearance(float sd, float sc, const float* x3, const float* y3, const float* z3, const unsigned char* r, const unsigned char* g, const unsigned char* b, float w, const SemiMetricFunction * function ) {
  float * feature = new float [N_*6];
  float sd_inv = 1.0/sd;
  float sc_inv = 1.0/sc;
  for( int i=0; i<N_; i++ ){
    feature[i*6+0] =  x3[i] *sd_inv;
    feature[i*6+1] =  y3[i] *sd_inv;
    feature[i*6+2] =  z3[i] *sd_inv;
    feature[i*6+3] =  static_cast<float>(r[i]) *sc_inv;
    feature[i*6+4] =  static_cast<float>(g[i]) *sc_inv;
    feature[i*6+5] =  static_cast<float>(b[i]) *sc_inv;
  }
  addPairwiseEnergy( feature, 6, w, function );
  delete [] feature;
}

void DenseCRF2D::Add3dNormals(float sd, float sn, const float* x3, const float* y3, const float* z3, const float* n_x3, const float* n_y3, const float* n_z3, float w, const SemiMetricFunction * function ) {
  float * feature = new float [N_*6];
  float sd_inv = 1.0/sd;
  float sn_inv = 1.0/sn;
  for( int i=0; i<N_; i++ ){
    feature[i*6+0] =  x3[i] *sd_inv;
    feature[i*6+1] =  y3[i] *sd_inv;
    feature[i*6+2] =  z3[i] *sd_inv;
    feature[i*6+3] =  n_x3[i] *sn_inv;
    feature[i*6+4] =  n_y3[i] *sn_inv;
    feature[i*6+5] =  n_z3[i] *sn_inv;
  }
  addPairwiseEnergy( feature, 6, w, function );
  delete [] feature;
}

void DenseCRF2D::Add3dAppearance(float sd, float sc, float sn, const float* x3, const float* y3, const float* z3, const unsigned char* r, const unsigned char* g, const unsigned char* b, const float* n_x3, const float* n_y3, const float* n_z3, float w, const SemiMetricFunction * function ) {
  float * feature = new float [N_*9];
  float sd_inv = 1.0/sd;
  float sc_inv = 1.0/sc;
  float sn_inv = 1.0f/sn;
  for( int i=0; i<N_; i++ ){
    feature[i*9+0] =  x3[i] *sd_inv;
    feature[i*9+1] =  y3[i] *sd_inv;
    feature[i*9+2] =  z3[i] *sd_inv;
    feature[i*9+3] =  static_cast<float>(r[i]) *sc_inv;
    feature[i*9+4] =  static_cast<float>(g[i]) *sc_inv;
    feature[i*9+5] =  static_cast<float>(b[i]) *sc_inv;
    feature[i*9+6] =  n_x3[i] *sn_inv;
    feature[i*9+7] =  n_y3[i] *sn_inv;
    feature[i*9+8] =  n_z3[i] *sn_inv;

  }
  addPairwiseEnergy( feature, 9, w, function );
  delete [] feature;
}


//////////////////////////////
/////  Unary Potentials  /////
//////////////////////////////
void DenseCRF::setUnaryEnergy ( const float* unary ) {
  memcpy( unary_, unary, N_*M_*sizeof(float) );
}
void DenseCRF::setUnaryEnergy ( int n, const float* unary ) {
  memcpy( unary_+n*M_, unary, M_*sizeof(float) );
}
void DenseCRF2D::setUnaryEnergy ( int x, int y, const float* unary ) {
  memcpy( unary_+(x+y*W_)*M_, unary, M_*sizeof(float) );
}


///////////////////////
/////  Inference  /////
///////////////////////
void DenseCRF::inference ( int n_iterations, float* result, float relax ) {
  // Run inference
  float * prob = runInference( n_iterations, relax );
  // Copy the result over
  for( int i=0; i<N_; i++ )
    memcpy( result+i*M_, prob+i*M_, M_*sizeof(float) );
}
void DenseCRF::map (int n_iterations, short* result, float relax) {
  // Run inference
  float * prob = runInference( n_iterations, relax );

  // Find the map
  for( int i=0; i<N_; i++ ){
    const float * p = prob + i*M_;
    // Find the max and subtract it so that the exp doesn't explode
    float mx = p[0];
    int imx = 0;
    for( int j=1; j<M_; j++ )
      if( mx < p[j] ){
        mx = p[j];
        imx = j;
      }
    result[i] = imx;
  }

}

float* DenseCRF::runInference( int n_iterations, float relax ) {
  startInference();
  for( int it=0; it<n_iterations; it++ )
    stepInference(relax);
  return current_;
}
void DenseCRF::expAndNormalize ( float* out, const float* in, float scale, float relax ) {
#pragma omp parallel for if(!m_do_not_parallelize)
  for( int i=0; i<N_; i++ ){
    const float * b = in + i*M_;
    // Find the max and subtract it so that the exp doesn't explode
    float mx = scale*b[0];
    float *V = new float[ M_+10 ];
    for( int j=0; j<M_; j++ ){
      float temp = scale*b[j];
      V[j] = temp;
      if( mx < temp )
        mx = temp;
    }
    float tt = 0;
    for( int j=0; j<M_; j++ ){
      V[j] = fast_exp( V[j]-mx );
      tt += V[j];
    }
    float tt_inv = 1.0f/tt;
    // Make it a probability
    for( int j=0; j<M_; j++ )
      V[j] *= tt_inv;

    float * a = out + i*M_;
    for( int j=0; j<M_; j++ )
      if (relax == 1)
        a[j] = V[j];
      else
        a[j] = (1-relax)*a[j] + relax*V[j];

    delete[] V;
  }
}
///////////////////
/////  Debug  /////
///////////////////

void DenseCRF::unaryEnergy(const short* ass, float* result) {
  for( int i=0; i<N_; i++ )
    if ( 0 <= ass[i] && ass[i] < M_ )
      result[i] = unary_[ M_*i + ass[i] ];
    else
      result[i] = 0;
}
void DenseCRF::pairwiseEnergy(const short* ass, float* result, int term) {
  float * current = allocate( N_*M_ );
  // Build the current belief [binary assignment]
  for( int i=0,k=0; i<N_; i++ )
    for( int j=0; j<M_; j++, k++ )
      current[k] = (ass[i] == j);

  for( int i=0; i<N_*M_; i++ )
    next_[i] = 0;
  if (term == -1)
    for( unsigned int i=0; i<pairwise_.size(); i++ )
      pairwise_[i]->apply( next_, current, tmp_, M_ );
  else
    pairwise_[ term ]->apply( next_, current, tmp_, M_ );
  for( int i=0; i<N_; i++ )
    if ( 0 <= ass[i] && ass[i] < M_ )
      result[i] =-next_[ i*M_ + ass[i] ];
    else
      result[i] = 0;
  deallocate( current );
}

void DenseCRF::StopParallelization(){
  m_do_not_parallelize = true;
}
void DenseCRF::startInference(){
  // Initialize using the unary energies
  expAndNormalize( current_, unary_, -1 );
}
void DenseCRF::stepInference( float relax ){
#ifdef SSE_DENSE_CRF
  __m128 * sse_next_ = (__m128*)next_;
  __m128 * sse_unary_ = (__m128*)unary_;
  __m128 * sse_additional_unary_ = (__m128*)additional_unary_;
#endif
  // Set the unary potential
#ifdef SSE_DENSE_CRF
  for( int i=0; i<(N_*M_-1)/4+1; i++ )

    sse_next_[i] = - sse_unary_[i] - sse_additional_unary_[i];
#else
  for( int i=0; i<N_*M_; i++ )
    next_[i] = -unary_[i] - additional_unary_[i];
#endif


  // Add up all pairwise potentials
  for( unsigned int i=0; i<pairwise_.size(); i++ )
    pairwise_[i]->apply( next_, current_, tmp_, M_ );


  // Exponentiate and normalize
  expAndNormalize( current_, next_, 1.0, relax );
}


void DenseCRF::stepInference(float * additional_messages, float relax ){
#ifdef SSE_DENSE_CRF
  __m128 * sse_next_ = (__m128*)next_;
  __m128 * sse_unary_ = (__m128*)unary_;
  __m128 * sse_additional_unary_ = (__m128*)additional_unary_;
  // __m128 * sse_penalties = (__m128*)consistency_penalty;
#endif
  // Set the unary potential
#ifdef SSE_DENSE_CRF
  for( int i=0; i<(N_*M_-1)/4+1; i++ )
    sse_next_[i] = - sse_unary_[i] - sse_additional_unary_[i];
#else
  for( int i=0; i<N_*M_; i++ )
    next_[i] = -unary_[i] - additional_unary_[i];
#endif




  // Add up all pairwise potentials
  for( unsigned int i=0; i<pairwise_.size(); i++ )
    pairwise_[i]->apply( next_, current_, tmp_, M_ );


  for( int i=0; i<N_*M_; i++ ){
    next_temp_[i] = next_[i] +unary_[i];
    next_[i] +=  additional_messages[i];

  }
  //  for( int i=0; i<N_; i+=100 ){
  //    for(int c=0; c< M_; ++c){
  //      std::cout << next_temp_[i] << "," << consistency_penalty[i] << "  ";
  //    }
  //    std::cout << std::endl;
  //  }

  // Exponentiate and normalize
  expAndNormalize( current_, next_, 1.0, relax );
}

float * DenseCRF::computeMessages(float *new_current){
#ifdef SSE_DENSE_CRF
  __m128 * sse_next_ = (__m128*)next_;
  __m128 * sse_unary_ = (__m128*)unary_;
  __m128 * sse_additional_unary_ = (__m128*)additional_unary_;
  __m128 * sse_current = (__m128*)current_;
  __m128 * sse_new_current = (__m128*)new_current;
#endif
  // Set the unary potential
#ifdef SSE_DENSE_CRF
  for( int i=0; i<(N_*M_-1)/4+1; i++ ){
    sse_current[i]= sse_new_current[i];
  }
#else
  for( int i=0; i<N_*M_; i++ ){
    current_[i] = new_current[i];
  }
#endif

  //TODO do this more efficient? sse? memset?
  for( int i=0; i<N_*M_; i++ ){
    next_[i] =0;
  }



  // Add up all pairwise potentials
  for( unsigned int i=0; i<pairwise_.size(); i++ )
    pairwise_[i]->apply( next_, current_, tmp_, M_ );


  return next_;
}


const float *DenseCRF::getCurrentProbability(){
  return current_;
}

const float *DenseCRF::getCurrentMessages(){
  return next_temp_;
}


void DenseCRF::currentMap( short * result ){
  // Find the map
  for( int i=0; i<N_; i++ ){
    const float * p = current_ + i*M_;
    // Find the max and subtract it so that the exp doesn't explode
    float mx = p[0];
    int imx = 0;
    for( int j=1; j<M_; j++ )
      if( mx < p[j] ){
        mx = p[j];
        imx = j;
      }
    result[i] = imx;
  }
}

