/*
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "batcher.h"
#include "common/rand.h"
#include "metrics.h"

#include <shared_mutex>

#include <glog/logging.h>

#include <autogradpp/autograd.h>

/**
 * The TorchCraftAI training library.
 */
namespace cpid {
// I'm unsure whether this is needed, some higher power needs to decide on how
// to
// hash individual "games" so they can be seen as separate. Games are separate
// from episodes because you can, for example, have multiple "episodes" of a
// Builder (starts when it tries to build something, ends when we know whether
// it succeeded or not) within the same game... For now we can probably just
// ignore it or pass in "" for all cases.
using GameUID = std::string;
using EpisodeKey = std::string;
const constexpr auto kDefaultEpisodeKey = "";
class Evaluator;

inline const std::string kValueKey = "V";
inline const std::string kQKey = "Q";
inline const std::string kPiKey = "Pi";
inline const std::string kSigmaKey = "std";
inline const std::string kActionQKey = "actionQ";
inline const std::string kActionKey = "action";
inline const std::string kPActionKey = "pAction";

struct pairhash {
 public:
  template <typename T, typename U>
  std::size_t operator()(const std::pair<T, U>& x) const {
    return std::hash<T>()(x.first) ^ std::hash<U>()(x.second);
  }
};

struct EpisodeTuple {
  GameUID gameID;
  EpisodeKey episodeKey;
};

// Creates UIDs for each rank, uint64_t unique ids (will wrap when exhausted)
GameUID genGameUID();

/**
 * Stub base class for replay buffer frames.
 *
 * Frames that are serializable need to implement serialize() or load()/save()
 * and register themselves in global scope with
 * `CEREAL_REGISTER_TYPE(MyReplayBufferFrame)` and (depending on how you
 * serialize) `CEREAL_REGISTER_POLYMORPHIC_RELATION(ReplayBufferFrame,
 * MyReplayBufferFrame)`
 */
struct ReplayBufferFrame {
  virtual ~ReplayBufferFrame() = default;

  template <class Archive>
  void serialize(Archive& ar) {}
};

struct RewardBufferFrame : ReplayBufferFrame {
  explicit RewardBufferFrame(float reward) : reward(reward) {}
  float reward;
};

/**
 * Stores an unordered_map[GameUID] = unordered_map[int, Episode]
 * All the public functions here should be autolocking and therefore relatively
 * thread safe. However, things like size do no perfectly accurately represent
 * the size in a multithreaded environment.
 */
class ReplayBuffer {
 public:
  using Episode = std::vector<std::shared_ptr<ReplayBufferFrame>>;
  using Store =
      std::unordered_map<GameUID, std::unordered_map<EpisodeKey, Episode>>;
  using UIDKeyStore =
      std::unordered_map<GameUID, std::unordered_set<EpisodeKey>>;
  using SampleOutput = std::pair<EpisodeTuple, std::reference_wrapper<Episode>>;

  Episode& append(
      GameUID uid,
      EpisodeKey key,
      std::shared_ptr<ReplayBufferFrame> value,
      bool isDone = false);

  std::size_t size() const;
  std::size_t size(GameUID const&) const;
  std::size_t sizeDone() const;
  std::size_t sizeDone(GameUID const&) const;
  void clear();
  void erase(GameUID const&, EpisodeKey const& = kDefaultEpisodeKey);
  std::vector<SampleOutput> getAllEpisodes();

  // Stupid sample for now, samples uniformly over games and then over episodes
  // No guarantee of uniqueness :)
  template <typename RandomGenerator>
  std::vector<SampleOutput> sample(RandomGenerator& g, uint32_t num = 1);
  std::vector<SampleOutput> sample(uint32_t num = 1);

  Episode& get(GameUID const&, EpisodeKey const& = kDefaultEpisodeKey);
  bool has(GameUID const&, EpisodeKey const& = kDefaultEpisodeKey);
  bool isDone(GameUID const&, EpisodeKey const& = kDefaultEpisodeKey);

 protected:
  Store storage_;
  UIDKeyStore dones_;

  template <typename RandomGenerator>
  SampleOutput sample_(RandomGenerator& g);

  // Can be replaced with shared_mutex in C++17
  mutable std::shared_timed_mutex replayerRWMutex_;
};

class AsyncBatcher;
class BaseSampler;

struct HandleGuard {};

/**
 * The Trainer should be shared amongst multiple different nodes, and
 * attached to a single Module.
 * It consists of an:
 *      - Algorithm, built into the trainer and subclassed via implementing
 *        the stepFrame and stepGame functions. The trainer itself might also
 *        start a seperate thread or otherwise have functionality for
 *        syncing weights.
 *      - Model, defined externally subject to algorithm specifications
 *        I don't know of a good way of enforcing model output, except an
 *        incorrect output spec will cause the algorithm to fail. Thus,
 *        please comment new algorithms with its input specification.
 *      - An optimizer, perhaps defined externally. Many of the multinode
 *        algorithms will probably force its own optimizer
 *      - A sampler, responsible for transforming the model output into an
 *        action
 *      - A replay buffer, as implemented below and forced by subclassing the
 *        algorithm. Each algorithm will expect its own replay buffer.
 *
 */
class Trainer {
 public:
  struct EpisodeHandle {
    EpisodeHandle(Trainer* trainer, GameUID id, EpisodeKey k);

    // No trainer = no handle
    EpisodeHandle() = default;
    explicit operator bool() const;

    GameUID const& gameID() const;
    EpisodeKey const& episodeKey() const;

    ~EpisodeHandle();

    // Can't copy or assign
    EpisodeHandle(EpisodeHandle&) = delete;
    EpisodeHandle(EpisodeHandle const&) = delete;
    EpisodeHandle& operator=(EpisodeHandle&) = delete;
    EpisodeHandle& operator=(EpisodeHandle const&) = delete;

    // Move is possible, and it will invalidate other episode
    EpisodeHandle(EpisodeHandle&&);
    // In move assignment, if we have a existing episode, it's force stopped
    EpisodeHandle& operator=(EpisodeHandle&&);

    friend std::ostream& operator<<(std::ostream&, EpisodeHandle const&);

   private:
    Trainer* trainer_;
    GameUID gameID_;
    EpisodeKey episodeKey_;
    std::weak_ptr<HandleGuard> guard_;
  };
  Trainer(
      ag::Container model,
      ag::Optimizer optim,
      std::unique_ptr<BaseSampler>,
      std::unique_ptr<AsyncBatcher> batcher = nullptr);
  virtual ag::Variant forward(ag::Variant inp, EpisodeHandle const&);

  /// Convenience function when one need to forward a single input. This will
  /// make it look batched and forward it, so that the model has no problem
  /// handling it.
  /// If \param{model} is not provided, will use the trainer's model_.
  ag::Variant forwardUnbatched(ag::Variant in, ag::Container model = nullptr);

  // Runs the training loop once. The return value is whether the model
  // succesfully updated. Sometimes, algorithms will be blocked while waiting
  // for new episodes, and this update will return false;
  virtual bool update() = 0;
  virtual ~Trainer() = default;

  void setTrain(bool = true);
  bool isTrain() const {
    return train_;
  }
  /// Sample using the class' sampler
  ag::Variant sample(ag::Variant in);
  ag::Container model() const;
  ag::Optimizer optim() const;
  ReplayBuffer& replayBuffer();
  virtual std::shared_ptr<Evaluator> makeEvaluator(
      size_t /* how many to run */,
      std::unique_ptr<BaseSampler> sampler);

  // These helper functions expose an atomic<bool> that can be convenient
  // for coordinating training threads. The Trainer itself is not affected by
  // this.
  void setDone(bool = true);
  bool isDone() const {
    return done_.load();
  }

  virtual void step(
      EpisodeHandle const&,
      std::shared_ptr<ReplayBufferFrame> v,
      bool isDone = false);
  virtual std::shared_ptr<ReplayBufferFrame>
  makeFrame(ag::Variant trainerOutput, ag::Variant state, float reward) = 0;
  /// Returns true if succeeded to register an episode, and false otherwise.
  /// After
  /// receiving false, a worker thread should check stopping conditins and
  /// re-try.
  virtual EpisodeHandle startEpisode();
  // For when an episode is not done and you want to remove it from training
  // because it is corrupted or for some other reason.
  virtual void forceStopEpisode(EpisodeHandle const&);
  bool isActive(EpisodeHandle const&);
  /// Releases all the worker threads so that they can be joined.
  /// For the off-policy trainers, labels all games as inactive. For the
  /// on-policy trainers, additionally un-blocks all threads that could be
  /// waiting at the batch barrier.
  virtual void reset();

  template <class Archive>
  void save(Archive& ar) const;
  template <class Archive>
  void load(Archive& ar);
  template <typename T>
  bool is() const;

  Trainer& setMetricsContext(std::shared_ptr<MetricsContext> context);
  std::shared_ptr<MetricsContext> metricsContext() const;

  TORCH_ARG(float, noiseStd) = 1e-2;
  TORCH_ARG(bool, continuousActions) = false;

  void setBatcher(std::unique_ptr<AsyncBatcher> batcher);

 protected:
  virtual void
  stepFrame(GameUID const&, EpisodeKey const&, ReplayBuffer::Episode&){};
  virtual void
  stepEpisode(GameUID const&, EpisodeKey const&, ReplayBuffer::Episode&){};
  // Currently, stepGame is not called from anywhere. TODO
  virtual void stepGame(GameUID const& game){};

  ag::Container model_;
  ag::Optimizer optim_;
  std::shared_ptr<MetricsContext> metricsContext_;
  ReplayBuffer replayer_;
  bool train_ = true;
  std::atomic<bool> done_{false};
  std::mutex modelWriteMutex_;
  std::shared_timed_mutex activeMapMutex_;

  std::unique_ptr<BaseSampler> sampler_;
  std::unique_ptr<AsyncBatcher> batcher_;

  std::shared_ptr<HandleGuard> epGuard_;
  template <typename T>
  std::vector<T const*> cast(ReplayBuffer::Episode const& e);

  using ForwardFunction =
      std::function<ag::Variant(ag::Variant, EpisodeHandle const&)>;
  // Private for trainers to use if they want to support evaluation
  static std::shared_ptr<Evaluator> evaluatorFactory(
      ag::Container model,
      std::unique_ptr<BaseSampler> s,
      size_t n,
      ForwardFunction func);

  ReplayBuffer::UIDKeyStore actives_;
  /// We subsample kFwdMetricsSubsampling of the forward() events
  /// when measuring their duration
  static constexpr float kFwdMetricsSubsampling = 0.1;
};
using EpisodeHandle = Trainer::EpisodeHandle;

/********************* IMPLEMENTATIONS *************************/

template <typename RandomGenerator>
inline std::vector<ReplayBuffer::SampleOutput> ReplayBuffer::sample(
    RandomGenerator& g,
    uint32_t num) {
  std::vector<SampleOutput> samples;
  for (uint32_t i = 0; i < num; i++) {
    samples.push_back(sample_(g));
  }
  return samples;
}

template <typename RandomGenerator>
inline ReplayBuffer::SampleOutput ReplayBuffer::sample_(RandomGenerator& g) {
  std::shared_lock<std::shared_timed_mutex> lock(replayerRWMutex_);
  if (dones_.size() == 0) {
    throw std::runtime_error("No finished episodes yet...");
  }
  auto& game = *common::select_randomly(dones_.begin(), dones_.end(), g);
  if (game.second.size() == 0) {
    LOG(FATAL) << "no episodes in game"; // This shouldn't ever happen...
  }
  auto& ep =
      *common::select_randomly(game.second.begin(), game.second.end(), g);
  return std::make_pair(
      EpisodeTuple{game.first, ep}, std::ref(storage_[game.first][ep]));
}

template <typename T>
inline std::vector<T const*> Trainer::cast(ReplayBuffer::Episode const& e) {
  // TODO might be better to return an iterator instead
  std::vector<T const*> ret;
  ret.reserve(e.size());
  for (auto& elem : e) {
    ret.push_back(static_cast<T const*>(elem.get()));
  }
  return ret;
}

template <class Archive>
inline void Trainer::save(Archive& ar) const {
  ar(CEREAL_NVP(*model_));
  ar(CEREAL_NVP(optim_));
}

template <class Archive>
inline void Trainer::load(Archive& ar) {
  ar(CEREAL_NVP(*model_));
  ar(CEREAL_NVP(optim_));
  optim_->add_parameters(model_->parameters());
}

template <typename T>
inline bool Trainer::is() const {
  return dynamic_cast<const T*>(this) != nullptr;
}

inline Trainer& Trainer::setMetricsContext(
    std::shared_ptr<MetricsContext> context) {
  metricsContext_ = context;
  return *this;
}

inline std::shared_ptr<MetricsContext> Trainer::metricsContext() const {
  return metricsContext_;
}
} // namespace cpid
