/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "ResourceInspector.h"
#include <QKeyEvent>
#include "Widgets/Extended/RDHeaderView.h"
#include "ui_ResourceInspector.h"

static const int ResourceIdRole = Qt::UserRole;
static const int FilterRole = Qt::UserRole + 1;

Q_DECLARE_METATYPE(ResourceId);

class ResourceListItemModel : public QAbstractItemModel
{
public:
  ResourceListItemModel(QWidget *parent, ICaptureContext &ctx)
      : QAbstractItemModel(parent), m_Ctx(ctx)
  {
  }

  void reset()
  {
    emit beginResetModel();
    emit endResetModel();
  }

  QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override
  {
    if(row < 0 || row >= rowCount())
      return QModelIndex();

    return createIndex(row, 0);
  }

  QModelIndex parent(const QModelIndex &index) const override { return QModelIndex(); }
  int rowCount(const QModelIndex &parent = QModelIndex()) const override
  {
    return m_Ctx.GetResources().count();
  }
  int columnCount(const QModelIndex &parent = QModelIndex()) const override { return 1; }
  Qt::ItemFlags flags(const QModelIndex &index) const override
  {
    if(!index.isValid())
      return 0;

    return QAbstractItemModel::flags(index);
  }

  QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
  {
    if(index.isValid())
    {
      const rdcarray<ResourceDescription> &resources = m_Ctx.GetResources();

      if(index.row() < resources.count())
      {
        const ResourceDescription &desc = resources[index.row()];

        if(role == Qt::DisplayRole)
          return m_Ctx.GetResourceName(desc.ID);

        if(role == ResourceIdRole)
          return QVariant::fromValue(desc.ID);

        if(role == FilterRole)
          return ToQStr(desc.type) + lit(" ") + m_Ctx.GetResourceName(desc.ID);
      }
    }

    return QVariant();
  }

private:
  ICaptureContext &m_Ctx;
};

ResourceInspector::ResourceInspector(ICaptureContext &ctx, QWidget *parent)
    : QFrame(parent), ui(new Ui::ResourceInspector), m_Ctx(ctx)
{
  ui->setupUi(this);

  ui->resourceName->setText(tr("No Resource Selected"));

  ui->resetName->hide();
  ui->resourceNameEdit->hide();
  ui->renameResource->setEnabled(false);

  ui->viewContents->hide();

  m_ResourceModel = new ResourceListItemModel(this, m_Ctx);

  m_FilterModel = new QSortFilterProxyModel(this);
  m_FilterModel->setSourceModel(m_ResourceModel);
  m_FilterModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
  m_FilterModel->setFilterRole(FilterRole);
  m_FilterModel->setSortCaseSensitivity(Qt::CaseInsensitive);
  m_FilterModel->setSortRole(Qt::DisplayRole);

  ui->resourceList->setModel(m_FilterModel);

  ui->initChunks->setColumns({lit("Parameter"), tr("Value")});
  ui->initChunks->header()->resizeSection(0, 200);

  ui->initChunks->setFont(Formatter::PreferredFont());
  ui->relatedResources->setFont(Formatter::PreferredFont());
  ui->resourceUsage->setFont(Formatter::PreferredFont());

  {
    RDHeaderView *header = new RDHeaderView(Qt::Horizontal, this);
    ui->relatedResources->setHeader(header);

    ui->relatedResources->setColumns({tr("Type"), tr("Resource")});
    header->setColumnStretchHints({-1, 1});

    ui->relatedResources->setClearSelectionOnFocusLoss(true);
  }

  ui->resourceUsage->setColumns({tr("EID"), tr("Usage")});

  QObject::connect(ui->resourceList, &QListView::doubleClicked, this,
                   &ResourceInspector::resource_doubleClicked);
  QObject::connect(ui->relatedResources, &QTreeView::doubleClicked, this,
                   &ResourceInspector::resource_doubleClicked);

  Inspect(ResourceId());

  m_Ctx.AddLogViewer(this);
}

ResourceInspector::~ResourceInspector()
{
  m_Ctx.BuiltinWindowClosed(this);
  m_Ctx.RemoveLogViewer(this);
  delete ui;
}

void ResourceInspector::Inspect(ResourceId id)
{
  if(m_Resource == id)
    return;

  m_Resource = id;

  ui->viewContents->setVisible(m_Ctx.GetTexture(id) || m_Ctx.GetBuffer(id));

  m_ResourceModel->reset();
  m_FilterModel->sort(0);

  if(m_Ctx.HasResourceCustomName(id))
    ui->resetName->show();
  else
    ui->resetName->hide();

  ui->initChunks->setUpdatesEnabled(false);
  ui->initChunks->clear();
  ui->relatedResources->clear();
  ui->resourceUsage->clear();

  const SDFile &file = m_Ctx.GetStructuredFile();
  const ResourceDescription *desc = m_Ctx.GetResource(id);

  m_Ctx.Replay().AsyncInvoke([this, id](IReplayController *r) {
    rdcarray<EventUsage> usage = r->GetUsage(id);

    GUIInvoke::call([this, id, usage] {
      CombineUsageEvents(
          m_Ctx, usage, [this, id](uint32_t startEID, uint32_t endEID, ResourceUsage use) {
            QString text;

            if(startEID == endEID)
              text = QFormatStr("EID %1").arg(startEID);
            else
              text = QFormatStr("EID %1-%2").arg(startEID).arg(endEID);

            RDTreeWidgetItem *item =
                new RDTreeWidgetItem({text, ToQStr(use, m_Ctx.APIProps().pipelineType)});
            item->setData(0, ResourceIdRole, QVariant(endEID));

            ui->resourceUsage->addTopLevelItem(item);
          });
    });
  });

  if(desc)
  {
    ui->resourceName->setText(m_Ctx.GetResourceName(id));

    for(ResourceId parent : desc->parentResources)
    {
      RDTreeWidgetItem *item = new RDTreeWidgetItem({tr("Parent"), m_Ctx.GetResourceName(parent)});
      item->setData(0, ResourceIdRole, QVariant::fromValue(parent));
      item->setData(1, ResourceIdRole, QVariant::fromValue(parent));
      ui->relatedResources->addTopLevelItem(item);
    }

    for(ResourceId derived : desc->derivedResources)
    {
      RDTreeWidgetItem *item = new RDTreeWidgetItem({tr("Derived"), m_Ctx.GetResourceName(derived)});
      item->setData(0, ResourceIdRole, QVariant::fromValue(derived));
      item->setData(1, ResourceIdRole, QVariant::fromValue(derived));
      ui->relatedResources->addTopLevelItem(item);
    }

    for(uint32_t chunk : desc->initialisationChunks)
    {
      RDTreeWidgetItem *root = new RDTreeWidgetItem({QString(), QString()});

      if(chunk < file.chunks.size())
      {
        SDChunk *chunkObj = file.chunks[chunk];

        root->setText(0, chunkObj->name);

        addStructuredObjects(m_Ctx, root, chunkObj->data.children, false);
      }
      else
      {
        root->setText(1, tr("Invalid chunk index %1").arg(chunk));
      }

      ui->initChunks->addTopLevelItem(root);

      ui->initChunks->setSelectedItem(root);
    }
  }
  else
  {
    m_Resource = ResourceId();
    ui->resourceName->setText(tr("No Resource Selected"));
  }

  ui->initChunks->setUpdatesEnabled(true);
}

void ResourceInspector::OnLogfileLoaded()
{
  ui->renameResource->setEnabled(true);

  m_ResourceModel->reset();
  m_FilterModel->sort(0);
}

void ResourceInspector::OnLogfileClosed()
{
  ui->renameResource->setEnabled(false);
  ui->resetName->hide();

  ui->resourceName->setText(tr("No Resource Selected"));

  ui->viewContents->hide();

  m_ResourceModel->reset();

  ui->initChunks->clear();
  ui->relatedResources->clear();
  ui->resourceUsage->clear();

  m_Resource = ResourceId();
}

void ResourceInspector::OnEventChanged(uint32_t eventID)
{
  Inspect(m_Resource);

  m_ResourceModel->reset();
  m_FilterModel->sort(0);
}

void ResourceInspector::on_renameResource_clicked()
{
  if(!ui->resourceNameEdit->isVisible())
  {
    ui->resourceNameEdit->setText(ui->resourceName->text());
    ui->resourceName->hide();
    ui->resourceNameEdit->show();
    ui->resourceNameEdit->setFocus();
  }
  else
  {
    // apply the edit
    ui->resourceName->setText(ui->resourceNameEdit->text());
    ui->resourceNameEdit->hide();
    ui->resourceName->show();

    ui->resetName->show();

    m_Ctx.SetResourceCustomName(m_Resource, ui->resourceName->text());
  }
}

void ResourceInspector::on_resourceNameEdit_keyPress(QKeyEvent *event)
{
  if(event->key() == Qt::Key_Escape)
  {
    // throw away the edit and show the name again
    ui->resourceNameEdit->hide();
    ui->resourceName->show();
  }
  else if(event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
  {
    // apply the edit
    on_renameResource_clicked();
  }
}

void ResourceInspector::on_resetName_clicked()
{
  ui->resourceName->setText(m_Ctx.GetResourceName(m_Resource));

  ui->resetName->hide();

  m_Ctx.SetResourceCustomName(m_Resource, QString());

  // force a refresh to pick up the new name
  ResourceId id = m_Resource;
  m_Resource = ResourceId();
  Inspect(id);
}

void ResourceInspector::on_cancelResourceListFilter_clicked()
{
  ui->resourceListFilter->setText(QString());
}

void ResourceInspector::on_resourceListFilter_textChanged(const QString &text)
{
  m_FilterModel->setFilterFixedString(text);
}

void ResourceInspector::resource_doubleClicked(const QModelIndex &index)
{
  ResourceId id = index.model()->data(index, ResourceIdRole).value<ResourceId>();
  Inspect(id);
}

void ResourceInspector::on_viewContents_clicked()
{
  TextureDescription *tex = m_Ctx.GetTexture(m_Resource);
  BufferDescription *buf = m_Ctx.GetBuffer(m_Resource);

  if(tex)
  {
    if(tex->resType == TextureDim::Buffer)
    {
      IBufferViewer *viewer = m_Ctx.ViewTextureAsBuffer(0, 0, tex->ID);

      m_Ctx.AddDockWindow(viewer->Widget(), DockReference::AddTo, this);
    }
    else
    {
      if(!m_Ctx.HasTextureViewer())
        m_Ctx.ShowTextureViewer();
      ITextureViewer *viewer = m_Ctx.GetTextureViewer();
      viewer->ViewTexture(tex->ID, true);
    }
  }
  else if(buf)
  {
    IBufferViewer *viewer = m_Ctx.ViewBuffer(0, buf->length, buf->ID);

    m_Ctx.AddDockWindow(viewer->Widget(), DockReference::AddTo, this);
  }
}

void ResourceInspector::on_resourceUsage_doubleClicked(const QModelIndex &index)
{
  uint32_t eid = index.model()->data(index, ResourceIdRole).value<uint32_t>();
  m_Ctx.SetEventID({}, eid, eid);
}
