#include "SearchGraph.h"
#include "moses/Manager.h"
#include "moses/Hypothesis.h"
#include "moses/FF/FeatureFunction.h"
#include "moses/FF/StatelessFeatureFunction.h"

#include <map>
#include <numeric>
#include <stdexcept>

namespace Moses
{

class SearchGraph::Edge::Impl
{
public:
  Impl() :
    m_begin(0), m_end(0),
    m_featureScores(),
    m_totalScore(std::numeric_limits<double>::infinity()),
    m_targetPhrase(),
    m_sourcePhrase(),
    m_sourceWordsRange(0, 0)
  {}

  Impl(const Impl& other) :
    m_begin(other.m_begin), m_end(other.m_end),
    m_featureScores(other.m_featureScores),
    m_totalScore(other.m_totalScore),
    m_targetPhrase(other.m_targetPhrase),
    m_sourcePhrase(other.m_sourcePhrase),
    m_sourceWordsRange(other.m_sourceWordsRange)
  {}

  void Init(VertexId begin, VertexId end, const Hypothesis& hypothesis,
      const std::vector<FeatureFunction*>& allFFs)
  {
    m_begin = begin;
    m_end = end;
    const Hypothesis& prevHypoth = *hypothesis.GetPrevHypo();
    m_totalScore = hypothesis.GetScore() - prevHypoth.GetScore();
    m_targetPhrase = hypothesis.GetCurrTargetPhrase();
    m_sourcePhrase =
        hypothesis.GetTranslationOption().GetInputPath().GetPhrase();
    m_sourceWordsRange = hypothesis.GetCurrSourceWordsRange();
    m_featureScores.resize(allFFs.size());
    const ScoreComponentCollection& currScores = hypothesis.GetScoreBreakdown();
    const ScoreComponentCollection& prevScores = prevHypoth.GetScoreBreakdown();
    for (size_t i = 0; i < allFFs.size(); ++i)
    {
      std::vector<float> vec1 = currScores.GetScoresForProducer(allFFs[i]);
      std::vector<float> vec2 = prevScores.GetScoresForProducer(allFFs[i]);
      m_featureScores[i].resize(allFFs[i]->GetNumScoreComponents());
      for (size_t j = 0; j < m_featureScores[i].size(); ++j)
      {
        m_featureScores[i][j] = vec1[j] - vec2[j];
      }
    }
  }

  VertexId Begin() const
  {
    return m_begin;
  }
  VertexId End() const
  {
    return m_end;
  }
  const std::vector<float>& FeatureScores(size_t featureId) const
  {
    return m_featureScores.at(featureId);
  }
  float TotalScore() const
  {
    return m_totalScore;
  }
  std::string GetSourceText() const {
    return FlattenPhrase(m_sourcePhrase);
  }
  std::string GetTargetText() const {
    return FlattenPhrase(m_targetPhrase);
  }

  WordsRange SourceWordsRange() const {
    return m_sourceWordsRange;
  }

  static std::string FlattenPhrase(const Phrase& phrase)
  {
    size_t size = phrase.GetSize();
    std::ostringstream oss;
    for (size_t i = 0; i < size; ++i)
    {
      const Word& w = phrase.GetWord(i);
      if (i != 0)
        oss << " ";
      oss << w.GetString(0);
    }
    return oss.str();
  }

  // fields
  VertexId m_begin; //! origin of the edge
  VertexId m_end;   //! end of the edge
  std::vector< std::vector<float> > m_featureScores;
  float m_totalScore;
  TargetPhrase m_targetPhrase;
  Phrase m_sourcePhrase;
  WordsRange m_sourceWordsRange;
};

class SearchGraph::BuilderBase
{
protected:
  SearchGraph& m_graph;
  const std::vector<FeatureFunction*>& m_featureFunctions;

  BuilderBase(SearchGraph& graph):
    m_graph(graph),
    m_featureFunctions(FeatureFunction::GetFeatureFunctionsForNow())
  {
    m_graph.m_featureDescriptions.resize(m_featureFunctions.size());
    m_graph.m_featureWeights.resize(m_featureFunctions.size());
    for (size_t i = 0; i < m_featureFunctions.size(); ++i)
    {
      m_graph.m_featureDescriptions[i] = m_featureFunctions[i]->GetScoreProducerDescription();
      m_graph.m_featureWeights[i] = StaticData::Instance().GetWeights(m_featureFunctions[i]);
    }
  }
};

class SearchGraph::BuilderFromManager: SearchGraph::BuilderBase
{
  typedef std::map<const Hypothesis*, VertexId> HypoMap;
  HypoMap m_hypoMap;
  VertexId m_vertexCount;
public:
  BuilderFromManager(SearchGraph& graph) :
      BuilderBase(graph),
      m_hypoMap(), m_vertexCount(0)
  {}

  void Build(const Manager& manager)
  {
    const HypothesisStack& lastStack = manager.GetLastSearchStack();
    PreliminaryEdgeList edges;
    for (HypothesisStack::const_iterator it = lastStack.begin();
        it != lastStack.end(); ++it)
    {
      MakePreliminaryEdgeList(*it, &edges);
    }
    VertexId sinkId = AddVertex(manager.GetBestHypothesis());
    for (size_t i = 0; i < edges.size(); ++i)
      AddEdge(edges[i].first, sinkId, *edges[i].second);
  }

private:
  VertexId DepthFirstSearch(const Hypothesis* hypo)
  {
    HypoMap::const_iterator hypoMapIter = m_hypoMap.find(hypo);
    if (hypoMapIter != m_hypoMap.end())
      return hypoMapIter->second;
    PreliminaryEdgeList edges;
    MakePreliminaryEdgeList(hypo, &edges);
    VertexId result = AddVertex(hypo);
    for (size_t i = 0; i < edges.size(); ++i)
      AddEdge(edges[i].first, result, *edges[i].second);
    return result;
  }

  typedef std::vector<std::pair<VertexId, const Hypothesis*> > PreliminaryEdgeList;

  size_t AddVertex(const Hypothesis* hypo)
  {
    VertexId result = m_vertexCount;
    m_hypoMap.insert(HypoMap::value_type(hypo, result));
    ++m_vertexCount;
    m_graph.m_incomingEdges.resize(m_vertexCount);
    m_graph.m_outgoingEdges.resize(m_vertexCount);
    return result;
  }

  void AddEdge(VertexId from, VertexId to, const Hypothesis& hypo)
  {
    size_t edgeId = m_graph.m_allEdges.size();
    m_graph.m_allEdges.push_back(SearchGraph::Edge());
    m_graph.m_allEdges.back().m_impl->Init(from, to, hypo, m_featureFunctions);
    m_graph.m_incomingEdges[to].push_back(edgeId);
    m_graph.m_outgoingEdges[from].push_back(edgeId);
  }

  void MakePreliminaryEdgeList(const Hypothesis* hypo,
      PreliminaryEdgeList* output)
  {
    if (hypo->GetId() == 0)
      return;
    output->push_back(
        PreliminaryEdgeList::value_type(DepthFirstSearch(hypo->GetPrevHypo()),
            hypo));
    const ArcList* arcList = hypo->GetArcList();
    if (arcList == NULL)
      return;
    for (ArcList::const_iterator it = arcList->begin(); it != arcList->end();
        ++it)
    {
      MakePreliminaryEdgeList(*it, output);
    }
  }
};

SearchGraph::SearchGraph(const Manager& manager)
{
  SearchGraph::BuilderFromManager builder(*this);
  builder.Build(manager);
}

class SearchGraph::BuilderFromGraph: SearchGraph::BuilderBase
{
  const SearchGraph& m_oldGraph;
  std::vector<int> m_featureReusage;
public:
  BuilderFromGraph(SearchGraph& newGraph, const SearchGraph& oldGraph):
    BuilderBase(newGraph),
    m_oldGraph(oldGraph)
  {}
  void Build() {
    PrepareReusage();
    m_graph.m_incomingEdges = m_oldGraph.m_incomingEdges;
    m_graph.m_outgoingEdges = m_oldGraph.m_outgoingEdges;
    m_graph.m_allEdges.reserve(m_oldGraph.m_allEdges.size());
    for (std::vector<Edge>::const_iterator it = m_oldGraph.m_allEdges.begin();
        it != m_oldGraph.m_allEdges.end(); ++it)
    {
      m_graph.m_allEdges.push_back(Edge(*it));
      Edge::Impl* e = m_graph.m_allEdges.back().m_impl;
      e->m_featureScores.clear();
      e->m_featureScores.resize(m_featureFunctions.size());
      for (size_t i = 0; i < m_featureFunctions.size(); ++i)
      {
        if (m_featureReusage[i] == -1)
        {
          e->m_featureScores[i] = CalculateFeatureScores(e, m_featureFunctions[i]);
        } else {
          e->m_featureScores[i] = it->FeatureScores(m_featureReusage[i]);
        }
      }
    }
  }
private:
  void PrepareReusage()
  {
    m_featureReusage.assign(m_featureFunctions.size(), -1);
    for (size_t i = 0; i < m_featureFunctions.size(); ++i)
    {
      std::vector<std::string>::const_iterator it = std::find(
          m_oldGraph.m_featureDescriptions.begin(),
          m_oldGraph.m_featureDescriptions.end(),
          m_graph.m_featureDescriptions[i]);
      if (it != m_oldGraph.m_featureDescriptions.end())
      {
        size_t j = it - m_oldGraph.m_featureDescriptions.begin();
        if (m_oldGraph.m_featureWeights[j].size() == m_graph.m_featureWeights[i].size())
        {
          m_featureReusage[i] = j;
        }
      }
    }
  }
  static std::vector<float> CalculateFeatureScores(const Edge::Impl* edge, FeatureFunction* ff)
  {
    if (!ff->IsStateless())
      throw new std::invalid_argument("can't rescore with a new stateful feature");
    std::vector<float> result;
    ScoreComponentCollection score, futureScore;
    ff->Evaluate(edge->m_sourcePhrase, edge->m_targetPhrase, score, futureScore);
    result = score.GetScoresForProducer(ff);
    return result;
  }
};

SearchGraph::SearchGraph(const SearchGraph& otherGraph)
{
  BuilderFromGraph builder(*this, otherGraph);
  builder.Build();
}

namespace
{
class IngoingOutgoingEdgesIter: public SearchGraph::EdgeIterator
{
  typedef SearchGraph::VertexId VertexId;
  const std::vector<SearchGraph::Edge>& m_edgeList;
  std::vector<VertexId>::const_iterator m_current, m_end;
public:
  IngoingOutgoingEdgesIter(const std::vector<SearchGraph::Edge>& edgeList,
      std::vector<VertexId>::const_iterator begin,
      std::vector<VertexId>::const_iterator end) :
        m_edgeList(edgeList), m_current(begin), m_end(end)
  {}

  virtual const SearchGraph::Edge& CurrentEdge() const
  {
    if (!Valid())
      abort();
    return m_edgeList[*m_current];
  }
  virtual void Next()
  {
    if (!Valid())
      abort();
    ++m_current;
  }
  virtual bool Valid() const
  {
    return (m_current != m_end);
  }
};

class AllEdgesIter : public SearchGraph::EdgeIterator
{
  typedef std::vector<SearchGraph::Edge>::const_iterator Iter;
  Iter m_current, m_end;
public:
  AllEdgesIter(const Iter& begin, const Iter& end):
    m_current(begin),
    m_end(end)
  {}

  virtual const SearchGraph::Edge& CurrentEdge() const
  {
    if (!Valid())
      abort();
    return *m_current;
  }
  virtual void Next()
  {
    if (!Valid())
      abort();
    ++m_current;
  }
  virtual bool Valid() const
  {
    return (m_current != m_end);
  }
};

} // anonymous namespace

std::auto_ptr<SearchGraph::EdgeIterator> SearchGraph::GetOutgoingEdgesIter(
  VertexId v) const
{
  return std::auto_ptr<SearchGraph::EdgeIterator>(new IngoingOutgoingEdgesIter(
      m_allEdges,
      m_outgoingEdges[v].begin(),
      m_outgoingEdges[v].end()));
}

std::auto_ptr<SearchGraph::EdgeIterator> SearchGraph::GetIncomingEdgesIter(
  VertexId v) const
{
  return std::auto_ptr<SearchGraph::EdgeIterator>(new IngoingOutgoingEdgesIter(
        m_allEdges,
        m_incomingEdges[v].begin(),
        m_incomingEdges[v].end()));
}

std::auto_ptr<SearchGraph::EdgeIterator> SearchGraph::GetAllEdgesIter() const
{
  return std::auto_ptr<SearchGraph::EdgeIterator>(new AllEdgesIter(
      m_allEdges.begin(), m_allEdges.end()));
}

std::string SearchGraph::FeatureDescription(size_t featureIndex) const
{
  return "";
}

const std::vector<float>& SearchGraph::FeatureWeights(size_t featureIndex) const
{
  return m_featureWeights[featureIndex];
}

void SearchGraph::UpdateEdgeScore(Edge* edge)
{
  Edge::Impl* impl = edge->m_impl;
  double totalScore = 0;
  for (size_t i = 0; i < m_featureWeights.size(); ++i)
    totalScore += std::inner_product(
        impl->m_featureScores[i].begin(), impl->m_featureScores[i].end(),
        m_featureWeights[i].begin(), 0.0f);
  impl->m_totalScore = totalScore;
}

SearchGraph::Edge::Edge():
    m_impl(new Impl())
{}

SearchGraph::Edge::Edge(const Edge& other):
    m_impl(new Impl(*other.m_impl))
{}

SearchGraph::Edge::~Edge()
{
  delete m_impl;
}

SearchGraph::VertexId SearchGraph::Edge::Begin() const
{
  return m_impl->Begin();
}

SearchGraph::VertexId SearchGraph::Edge::End() const
{
  return m_impl->End();
}

const std::vector<float>& SearchGraph::Edge::FeatureScores(
    size_t featureId) const
{
  return m_impl->FeatureScores(featureId);
}

float SearchGraph::Edge::TotalScore() const
{
  return m_impl->TotalScore();
}

std::string SearchGraph::Edge::GetSourceText() const
{
  return m_impl->GetSourceText();
}

std::string SearchGraph::Edge::GetTargetText() const
{
  return m_impl->GetTargetText();
}

WordsRange SearchGraph::Edge::SourceWordsRange() const
{
  return m_impl->SourceWordsRange();
}

void SearchGraph::Search(size_t pass)
{
	const std::vector<const StatelessFeatureFunction*> &slff = StatelessFeatureFunction::GetStatelessFeatureFunctions(pass);

	std::vector<const StatelessFeatureFunction*>::const_iterator iter;
	for (iter = slff.begin(); iter != slff.end(); ++iter) {
		const StatelessFeatureFunction &ff = **iter;
		// call Evaluate for all edges here

		ff.DoNow();
	}

	for (iter = slff.begin(); iter != slff.end(); ++iter) {
		const StatelessFeatureFunction &ff = **iter;
		// call Evaluate for all edges here

		ff.WaitUntilFinish();
	}
}

} // namespace Moses
